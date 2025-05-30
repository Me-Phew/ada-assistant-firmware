#include "esp_err.h"
#include "esp_log.h"
#include <driver/i2s_std.h>
#include <algorithm>

#include "ada_microphone_driver.hpp"
#include <esp_timer.h>

namespace ada_assistant
{
    namespace microphone_driver
    {

        static const char *TAG = "ADA_MIC_DRIVER";

        AdaMicrophoneDriver::AdaMicrophoneDriver() : bck_pin((gpio_num_t)CONFIG_ADA_MICROPHONE_DRIVER_SCK_GPIO),
                                                     sd_pin((gpio_num_t)CONFIG_ADA_MICROPHONE_DRIVER_SD_GPIO),
                                                     ws_pin((gpio_num_t)CONFIG_ADA_MICROPHONE_DRIVER_WS_GPIO),
                                                     i2s_port((i2s_port_t)CONFIG_ADA_MICROPHONE_DRIVER_I2S_PORT),
                                                     sample_rate(CONFIG_ADA_MICROPHONE_DRIVER_SAMPLE_RATE),
                                                     bits_per_sample((i2s_data_bit_width_t)CONFIG_ADA_MICROPHONE_DRIVER_BITS_PER_SAMPLE),
                                                     microphone_(),
                                                     is_recording_(false)
        {
        }

        AdaMicrophoneDriver::AdaMicrophoneDriver(ada_microphone_config_t config) : bck_pin(config.bck_pin),
                                                                                   sd_pin(config.sd_pin),
                                                                                   ws_pin(config.ws_pin),
                                                                                   i2s_port(config.i2s_port),
                                                                                   sample_rate(config.sample_rate),
                                                                                   bits_per_sample(config.bits_per_sample),
                                                                                   microphone_(),
                                                                                   is_recording_(false)
        {
        }

        esp_err_t AdaMicrophoneDriver::init(esp_event_loop_handle_t app_event_loop_handle)
        {
            esp_err_t ret_val = ESP_OK;

            app_event_loop_handle_ = app_event_loop_handle;

            ESP_LOGI(TAG, "Initializing microphone driver...");
            ESP_LOGI(TAG, "BCK Pin: %d", this->bck_pin);
            ESP_LOGI(TAG, "WS Pin: %d", this->ws_pin);
            ESP_LOGI(TAG, "SD Pin: %d", this->sd_pin);

            ESP_LOGI(TAG, "I2S Port: %d", this->i2s_port);

            ESP_LOGI(TAG, "Sample Rate: %d", this->sample_rate);
            ESP_LOGI(TAG, "Bits per Sample: %d", this->bits_per_sample);

            this->microphone_.set_bclk_pin(this->bck_pin);
            this->microphone_.set_lrclk_pin(this->ws_pin);
            this->microphone_.set_din_pin(this->sd_pin);

            i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(this->i2s_port, I2S_ROLE_MASTER);
            chan_config.dma_desc_num = 12;
            chan_config.dma_frame_num = 512;

            this->microphone_.set_channel(chan_config);

            this->microphone_.set_sample_rate(this->sample_rate);
            this->microphone_.set_bits_per_sample(this->bits_per_sample);

            return ret_val;
        }

        void AdaMicrophoneDriver::setup()
        {
            this->microphone_.setup();
        }

        void AdaMicrophoneDriver::start()
        {
            this->microphone_.start();
        }

        void AdaMicrophoneDriver::stop()
        {
            this->microphone_.stop();
        }

        void AdaMicrophoneDriver::loop()
        {
            this->microphone_.loop();
        }

        size_t AdaMicrophoneDriver::read(int16_t *buf, size_t len)
        {
            return this->microphone_.read(buf, len);
        }

        bool AdaMicrophoneDriver::is_running()
        {
            return this->microphone_.is_running();
        }

        bool AdaMicrophoneDriver::is_stopped()
        {
            return this->microphone_.is_stopped();
        }

        bool AdaMicrophoneDriver::is_recording()
        {
            return this->is_recording_;
        }

        esp_err_t AdaMicrophoneDriver::start_recording()
        {
            if (this->is_recording_)
            {
                ESP_LOGW(TAG, "Recording is already in progress.");
                return false;
            }

            BaseType_t task_created = xTaskCreatePinnedToCore(
                recording_task,
                "ada_recording_task",
                8192,
                this,
                10,
                &this->recording_task_handle_,
                1);

            if (task_created != pdPASS || this->recording_task_handle_ == NULL)
            {
                ESP_LOGE(TAG, "Failed to create recording task!");
                this->recording_task_handle_ = NULL;

                return ESP_FAIL;
            }

            ESP_LOGI(TAG, "Recording task created successfully.");

            esp_err_t ret = esp_event_post_to(this->app_event_loop_handle_,
                                              ADA_APP_EVENT_BASE,
                                              APP_EVENT_COMMAND_RECORDING_STARTED,
                                              NULL,
                                              0,
                                              portMAX_DELAY);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to post APP_EVENT_COMMAND_RECORDING_STARTED: %s", esp_err_to_name(ret));
            }

            return ret;
        }

        void AdaMicrophoneDriver::recording_task(void *params)
        {
            AdaMicrophoneDriver *self = static_cast<AdaMicrophoneDriver *>(params);

            ESP_LOGI(TAG, "Recording processing task started. Current Tick: %lu", xTaskGetTickCount());

            // Initial microphone start attempt
            if (!self->microphone_.is_running())
            {
                ESP_LOGI(TAG, "Microphone not running, attempting to start it initially.");
                self->microphone_.start();
            }
            else
            {
                ESP_LOGI(TAG, "Microphone already running at task start.");
            }

            // --- Allocate Buffer from HEAP ---
            int16_t *recording_buffer = NULL; // Pointer for the buffer

            int sample_size = self->bits_per_sample / 8;
            size_t samples_to_record = self->sample_rate * self->total_recording_duration_s;
            size_t recording_buffer_size = samples_to_record * sample_size;

            ESP_LOGI(TAG, "Attempting to allocate %zu bytes from PSRAM heap...", recording_buffer_size);
            recording_buffer = (int16_t *)heap_caps_malloc(recording_buffer_size, MALLOC_CAP_SPIRAM);

            // Check if allocation failed
            if (recording_buffer == NULL)
            {
                ESP_LOGE(TAG, "FAILED to allocate recording buffer from PSRAM heap!");
                return;
            }

            // --- If Allocation Succeeded, proceed ---
            ESP_LOGI(TAG, "PSRAM Buffer allocated successfully at %p.", recording_buffer);
            ESP_LOGI(TAG, "Starting audio recording for %lu seconds (%zu bytes)...", self->total_recording_duration_s, recording_buffer_size);

            // 1. Read data into Heap Buffer
            memset(recording_buffer, 0, recording_buffer_size); // Clear buffer
            size_t total_bytes_read = 0;
            uint32_t recording_start_time_us = esp_timer_get_time();
            const uint32_t recording_timeout_us = (recording_buffer_size / (sample_size * self->sample_rate)) * 1000000;

            const size_t CHUNK_SIZE = 512;
            const TickType_t READ_DELAY = pdMS_TO_TICKS(10);
            total_bytes_read = 0;

            ESP_LOGI(TAG, "Starting audio recording...");

            while (total_bytes_read < recording_buffer_size)
            {
                size_t remaining_bytes = recording_buffer_size - total_bytes_read;
                size_t bytes_to_read = std::min(remaining_bytes, CHUNK_SIZE);

                int16_t *current_buffer_ptr = recording_buffer + (total_bytes_read / sizeof(int16_t));

                size_t bytes_read_chunk = self->microphone_.read(current_buffer_ptr, bytes_to_read);

                if (bytes_read_chunk > 0)
                {
                    // ESP_LOGI(TAG, "Read %zu bytes this chunk, total so far: %zu / %zu",
                    //          bytes_read_chunk, total_bytes_read + bytes_read_chunk, recording_buffer_size);
                    total_bytes_read += bytes_read_chunk;
                }
                else
                {
                    ESP_LOGW(TAG, "Microphone returned 0 bytes, waiting...");
                    vTaskDelay(READ_DELAY);
                }

                int64_t elapsed_us = esp_timer_get_time() - recording_start_time_us;
                if (elapsed_us > recording_timeout_us)
                {
                    ESP_LOGW(TAG, "Recording timed out after %lld us! Read %zu bytes.", elapsed_us, total_bytes_read);
                    break;
                }
            }

            ESP_LOGI(TAG, "Finished recording. Read %zu bytes total.", total_bytes_read);
            int64_t total_time_us = esp_timer_get_time() - recording_start_time_us;
            float seconds = total_time_us / 1000000.0;
            float effective_sample_rate = (float)(total_bytes_read / sizeof(int16_t)) / seconds;

            ESP_LOGI(TAG, "Effective duration: %.2f seconds", seconds);
            ESP_LOGI(TAG, "Effective sample rate: %.2f samples/sec", effective_sample_rate);

            // Prepare event data
            event_audio_data_t event_payload;
            event_payload.buffer_ptr = recording_buffer;
            event_payload.bytes_recorded = total_bytes_read;
            event_payload.buffer_capacity = recording_buffer_size;
            event_payload.sample_rate = self->sample_rate;
            event_payload.bits_per_sample = self->bits_per_sample;

            bool event_posted_successfully = false;

            if (self->app_event_loop_handle_ != nullptr)
            {
                ESP_LOGI(TAG, "Posting APP_EVENT_COMMAND_RECORDING_FINISHED with data (buffer: %p, bytes_recorded: %zu, capacity: %zu)",
                         event_payload.buffer_ptr, event_payload.bytes_recorded, event_payload.buffer_capacity);

                esp_err_t ret = esp_event_post_to(self->app_event_loop_handle_,
                                                  ADA_APP_EVENT_BASE,
                                                  APP_EVENT_COMMAND_RECORDING_FINISHED,
                                                  &event_payload,        // Pass pointer to the struct
                                                  sizeof(event_payload), // Pass size of the struct
                                                  portMAX_DELAY);
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to post APP_EVENT_COMMAND_RECORDING_FINISHED: %s", esp_err_to_name(ret));
                }
                else
                {
                    ESP_LOGI(TAG, "Posted APP_EVENT_COMMAND_RECORDING_FINISHED successfully.");

                    event_posted_successfully = true;
                    // Buffer ownership is now transferred to the event handler.
                    // Nullify the pointer in this task's context.
                    recording_buffer = NULL;
                }
            }
            else
            {
                ESP_LOGW(TAG, "app_event_loop_handle_ is NULL, cannot post FINISHED event.");
            }

            if (!event_posted_successfully && recording_buffer != NULL)
            {
                ESP_LOGW(TAG, "Recording data not posted via event. Freeing recording_buffer %p.", recording_buffer);
                heap_caps_free(recording_buffer);
                recording_buffer = NULL; // Defensive nullification
            }

            if (self->microphone_.is_running())
            {
                ESP_LOGI(TAG, "Microphone is running. Stopping it.");
                self->microphone_.stop();
                ESP_LOGI(TAG, "Microphone stop called. Is running now: %d", (int)self->microphone_.is_running());
            }
            else
            {
                ESP_LOGI(TAG, "Microphone was not running at cleanup start.");
            }

            self->is_recording_ = false;
            ESP_LOGI(TAG, "Set self->is_recording_ to false. Value: %d", (int)self->is_recording_);

            ESP_LOGI(TAG, "Recording processing task finished. Preparing to delete task.");

            if (self->recording_task_handle_ == xTaskGetCurrentTaskHandle())
            {
                ESP_LOGI(TAG, "Clearing self->recording_task_handle_ as it matches current task.");
                self->recording_task_handle_ = NULL;
            }
            else if (self->recording_task_handle_ != NULL)
            {
                ESP_LOGW(TAG, "self->recording_task_handle_ (%p) does not match current task handle (%p). Not clearing it from class instance.",
                         self->recording_task_handle_, xTaskGetCurrentTaskHandle());
            }
            else
            {
                ESP_LOGI(TAG, "self->recording_task_handle_ was already NULL.");
            }

            ESP_LOGI(TAG, "Now self-deleting task. Goodbye!");
            vTaskDelete(NULL); // Self-delete
        }

        // void AdaMicrophoneDriver::recording_task(void *params) // Renamed from recording_task_entry to match your provided signature
        // {
        //     AdaMicrophoneDriver *self = static_cast<AdaMicrophoneDriver *>(params);

        //     if (!self)
        //     {
        //         // Cannot use self->TAG here. Define a static tag for this specific case or use a generic one.
        //         static const char *NULL_SELF_TAG = "AdaMicRecTaskErr";
        //         ESP_LOGE(NULL_SELF_TAG, "recording_task received NULL params. Task cannot run and will self-delete.");
        //         vTaskDelete(NULL);
        //         return;
        //     }

        //     ESP_LOGI(TAG, "Recording processing task started. Current Tick: %lu", xTaskGetTickCount());

        //     // Initial microphone start attempt
        //     if (!self->microphone_.is_running())
        //     {
        //         ESP_LOGI(TAG, "Microphone not running, attempting to start it initially.");
        //         self->microphone_.start();
        //     }
        //     else
        //     {
        //         ESP_LOGI(TAG, "Microphone already running at task start.");
        //     }

        //     bool continue_recording_loop = true;
        //     self->is_recording_ = true; // Explicitly set by the task
        //     ESP_LOGI(TAG, "Set self->is_recording_ to true. Value: %d", (int)self->is_recording_);

        //     event_audio_chunk_data_t *chunk_event_payload_ptr = nullptr;

        //     // --- Encapsulate main logic in a do-while(false) for cleanup ---
        //     do
        //     {
        //         ESP_LOGI(TAG, "Entering do-while(false) block.");
        //         // --- Parameter Validation and Initial Setup ---
        //         if (self->app_event_loop_handle_ == nullptr)
        //         {
        //             ESP_LOGE(TAG, "app_event_loop_handle_ is NULL. Breaking from do-while.");
        //             break; // Jump to cleanup
        //         }
        //         ESP_LOGI(TAG, "app_event_loop_handle_ is valid.");

        //         // Re-check/ensure microphone is running
        //         if (!self->microphone_.is_running())
        //         {
        //             ESP_LOGW(TAG, "Microphone not running inside do-while, attempting to start again.");
        //             self->microphone_.start();
        //         }
        //         ESP_LOGI(TAG, "Microphone is_running: %d", (int)self->microphone_.is_running());

        //         const size_t samples_per_target_chunk = (self->sample_rate * self->target_chunk_duration_ms) / 1000;
        //         if (samples_per_target_chunk == 0)
        //         {
        //             ESP_LOGE(TAG, "Calculated samples_per_target_chunk is 0. Check audio parameters. Breaking from do-while.");
        //             break; // Jump to cleanup
        //         }
        //         ESP_LOGI(TAG, "Target chunk: %zu samples (%lu ms)", samples_per_target_chunk, self->target_chunk_duration_ms);

        //         const size_t max_total_samples_for_session = (self->sample_rate * self->total_recording_duration_ms) / 1000;
        //         if (max_total_samples_for_session == 0 && self->total_recording_duration_ms > 0)
        //         {
        //             ESP_LOGE(TAG, "Calculated max_total_samples_for_session is 0. Check audio parameters. Breaking from do-while.");
        //             break; // Jump to cleanup
        //         }
        //         ESP_LOGI(TAG, "Total recording limit: %zu samples (%lu ms)", max_total_samples_for_session, self->total_recording_duration_ms);

        //         ESP_LOGI(TAG, "Attempting to allocate chunk_event_payload_ptr. Size: %zu bytes", sizeof(event_audio_chunk_data_t));
        //         chunk_event_payload_ptr = static_cast<event_audio_chunk_data_t *>(heap_caps_malloc(sizeof(event_audio_chunk_data_t), MALLOC_CAP_DEFAULT));
        //         if (!chunk_event_payload_ptr)
        //         {
        //             ESP_LOGE(TAG, "Failed to allocate memory for chunk_event_payload. Breaking from do-while.");
        //             break; // Jump to cleanup
        //         }
        //         ESP_LOGI(TAG, "Successfully allocated chunk_event_payload_ptr.");

        //         // --- Buffer Initialization ---
        //         ESP_LOGI(TAG, "Initializing mic_read_buffer. Samples: %zu", self->mic_read_buffer_samples);
        //         std::vector<int16_t> mic_read_buffer(self->mic_read_buffer_samples);
        //         ESP_LOGI(TAG, "Initializing accumulation_buffer. Reserved for: %zu samples", samples_per_target_chunk + self->mic_read_buffer_samples);
        //         std::vector<int16_t> accumulation_buffer;
        //         accumulation_buffer.reserve(samples_per_target_chunk + self->mic_read_buffer_samples);

        //         size_t total_samples_recorded_this_session = 0;

        //         ESP_LOGI(TAG, "Starting recording loop. Will record up to %zu samples.", max_total_samples_for_session);
        //         ESP_LOGI(TAG, "Loop Pre-Check: is_recording_=%d, mic_running=%d, continue_loop=%d, duration_check_passed=%d",
        //                  (int)self->is_recording_,
        //                  (int)self->microphone_.is_running(),
        //                  (int)continue_recording_loop,
        //                  (int)(self->total_recording_duration_ms == 0 || total_samples_recorded_this_session < max_total_samples_for_session));

        //         // --- Main Recording Loop ---
        //         while (self->is_recording_ && self->microphone_.is_running() && continue_recording_loop &&
        //                (self->total_recording_duration_ms == 0 || total_samples_recorded_this_session < max_total_samples_for_session))
        //         {
        //             // ESP_LOGI(TAG, "Top of main recording loop. total_samples_recorded: %zu", total_samples_recorded_this_session); // LOG 1

        //             size_t samples_to_read_from_mic = mic_read_buffer.size();
        //             if (self->total_recording_duration_ms > 0)
        //             {
        //                 size_t remaining_samples_for_session = max_total_samples_for_session - total_samples_recorded_this_session;
        //                 if (samples_to_read_from_mic > remaining_samples_for_session)
        //                 {
        //                     samples_to_read_from_mic = remaining_samples_for_session;
        //                 }
        //             }
        //             // ESP_LOGI(TAG, "Attempting to read %zu samples from mic.", samples_to_read_from_mic); // LOG 2

        //             if (samples_to_read_from_mic == 0 && self->total_recording_duration_ms > 0)
        //             {
        //                 // ESP_LOGI(TAG, "Reached max total samples for session (%zu). Breaking main loop.", max_total_samples_for_session);
        //                 break; // Exit the while loop, session complete by duration
        //             }

        //             // ESP_LOGI(TAG, "Calling microphone_.read().");
        //             size_t samples_actually_read = self->microphone_.read(mic_read_buffer.data(), std::max(samples_to_read_from_mic, (size_t)50));
        //             // ESP_LOGI(TAG, "Actually read %zu samples from mic.", samples_actually_read); // LOG 3

        //             if (samples_actually_read > 0)
        //             {
        //                 total_samples_recorded_this_session += samples_actually_read;
        //                 // ESP_LOGI(TAG, "Before acc_buffer.insert. acc_buffer.size: %zu, capacity: %zu. Adding %zu samples. New total_session_samples: %zu",
        //                 //  accumulation_buffer.size(), accumulation_buffer.capacity(), samples_actually_read, total_samples_recorded_this_session); // LOG 4
        //                 accumulation_buffer.insert(accumulation_buffer.end(),
        //                                            mic_read_buffer.begin(),
        //                                            mic_read_buffer.begin() + samples_actually_read);
        //                 // ESP_LOGI(TAG, "After acc_buffer.insert. acc_buffer.size: %zu", accumulation_buffer.size()); // LOG 5

        //                 while (accumulation_buffer.size() >= samples_per_target_chunk)
        //                 {
        //                     // ESP_LOGI(TAG, "Top of inner chunking loop. acc_buffer.size: %zu vs target_chunk_samples: %zu",
        //                     //  accumulation_buffer.size(), samples_per_target_chunk); // LOG 6
        //                     if (!self->is_recording_ || !continue_recording_loop)
        //                     {
        //                         ESP_LOGI(TAG, "Inner loop break: is_recording_=%d or continue_recording_loop=%d",
        //                                  (int)self->is_recording_, (int)continue_recording_loop);
        //                         break;
        //                     }

        //                     size_t bytes_to_post = samples_per_target_chunk * sizeof(int16_t);
        //                     // ESP_LOGI(TAG, "Accumulated %zu samples, forming %zu-byte chunk.", accumulation_buffer.size(), bytes_to_post);

        //                     if (bytes_to_post == 0)
        //                     {
        //                         // ESP_LOGW(TAG, "Attempting to post a zero-byte chunk. Skipping.");
        //                     }
        //                     else if (bytes_to_post > MAX_EVENT_AUDIO_BYTES)
        //                     {
        //                         // ESP_LOGE(TAG, "Audio data size %zu bytes exceeds MAX_EVENT_AUDIO_BYTES %d. Stopping.",
        //                         //  bytes_to_post, MAX_EVENT_AUDIO_BYTES);
        //                         accumulation_buffer.clear();
        //                         continue_recording_loop = false; // Signal outer loop to stop
        //                         break;                           // Break from inner while
        //                     }
        //                     else
        //                     {
        //                         // ESP_LOGI(TAG, "Preparing to post chunk. Bytes: %zu", bytes_to_post);
        //                         chunk_event_payload_ptr->actual_data_len_bytes = bytes_to_post;
        //                         memcpy(chunk_event_payload_ptr->audio_data, accumulation_buffer.data(), bytes_to_post);
        //                         size_t total_payload_size = offsetof(event_audio_chunk_data_t, audio_data) + bytes_to_post;

        //                         // ESP_LOGI(TAG, "Calling esp_event_post_to for chunk.");
        //                         esp_err_t ret = esp_event_post_to(self->app_event_loop_handle_,
        //                                                           ADA_APP_EVENT_BASE, // Make sure ADA_APP_EVENT_BASE is correctly defined/accessible
        //                                                           APP_EVENT_COMMAND_RECORDING_CHUNK_READY,
        //                                                           chunk_event_payload_ptr,
        //                                                           total_payload_size,
        //                                                           portMAX_DELAY);
        //                         if (ret != ESP_OK)
        //                         {
        //                             // ESP_LOGE(TAG, "Failed to post recorded data chunk: %s. Stopping.", esp_err_to_name(ret));
        //                             continue_recording_loop = false; // Signal outer loop to stop
        //                             break;                           // Break from inner while
        //                         }
        //                         else
        //                         {
        //                             // ESP_LOGI(TAG, "Posted chunk: %zu audio bytes (total event payload %zu bytes). Total session samples: %zu",
        //                             //  bytes_to_post, total_payload_size, total_samples_recorded_this_session);
        //                         }
        //                     }
        //                     // ESP_LOGI(TAG, "Before acc_buffer.erase. acc_buffer.size: %zu, erasing %zu samples.",
        //                     //  accumulation_buffer.size(), samples_per_target_chunk); // LOG 7
        //                     accumulation_buffer.erase(accumulation_buffer.begin(),
        //                                               accumulation_buffer.begin() + samples_per_target_chunk);
        //                     // ESP_LOGI(TAG, "After acc_buffer.erase. acc_buffer.size: %zu", accumulation_buffer.size()); // LOG 8
        //                 } // End inner while (chunk processing)
        //             }
        //             else if (samples_actually_read == 0)
        //             {
        //                 // ESP_LOGI(TAG, "No data read from microphone this cycle."); // LOG for 0 read
        //             }
        //             // ESP_LOGI(TAG, "Before vTaskDelay. Delay ticks: %lu", self->read_loop_delay_ticks); // LOG 9
        //             vTaskDelay(self->read_loop_delay_ticks);
        //             // ESP_LOGI(TAG, "After vTaskDelay."); // LOG 10

        //         } // End main recording while loop

        //         // ESP_LOGI(TAG, "Exited main recording loop."); // LOG 11

        //         // --- Post-Loop: Handle any remaining partial data ---
        //         // ESP_LOGI(TAG, "Post-loop check: is_recording_=%d, continue_loop=%d, acc_buffer_empty=%d, acc_buffer_size=%zu", // LOG 12
        //         //          (int)self->is_recording_,
        //         //          (int)continue_recording_loop,
        //         //          (int)accumulation_buffer.empty(),
        //         //          accumulation_buffer.size());

        //         if (self->is_recording_ && continue_recording_loop && !accumulation_buffer.empty())
        //         {
        //             // ESP_LOGI(TAG, "Attempting to post final partial chunk."); // LOG 13
        //             size_t bytes_to_post_final = accumulation_buffer.size() * sizeof(int16_t);
        //             // ESP_LOGI(TAG, "Posting remaining %zu samples (%zu bytes) as final partial chunk.",
        //             //  accumulation_buffer.size(), bytes_to_post_final);

        //             if (bytes_to_post_final == 0)
        //             {
        //                 // ESP_LOGW(TAG, "Final partial chunk is zero bytes. Skipping post.");
        //             }
        //             else if (bytes_to_post_final > MAX_EVENT_AUDIO_BYTES)
        //             {
        //                 // ESP_LOGE(TAG, "Final partial chunk audio data size %zu bytes exceeds MAX_EVENT_AUDIO_BYTES %d. Skipping.",
        //                 //  bytes_to_post_final, MAX_EVENT_AUDIO_BYTES);
        //             }
        //             else
        //             {
        //                 // ESP_LOGI(TAG, "Preparing to post final partial chunk. Bytes: %zu", bytes_to_post_final);
        //                 chunk_event_payload_ptr->actual_data_len_bytes = bytes_to_post_final;
        //                 memcpy(chunk_event_payload_ptr->audio_data, accumulation_buffer.data(), bytes_to_post_final);
        //                 size_t total_payload_size_final = offsetof(event_audio_chunk_data_t, audio_data) + bytes_to_post_final;

        //                 // ESP_LOGI(TAG, "Calling esp_event_post_to for final partial chunk.");
        //                 esp_err_t ret = esp_event_post_to(self->app_event_loop_handle_,
        //                                                   ADA_APP_EVENT_BASE,
        //                                                   APP_EVENT_COMMAND_RECORDING_CHUNK_READY,
        //                                                   chunk_event_payload_ptr,
        //                                                   total_payload_size_final,
        //                                                   portMAX_DELAY);
        //                 // if (ret != ESP_OK)
        //                 // {
        //                 //     ESP_LOGE(TAG, "Failed to post final partial data chunk: %s", esp_err_to_name(ret));
        //                 // }
        //                 // else
        //                 // {
        //                 //     ESP_LOGI(TAG, "Posted final partial chunk: %zu audio bytes (total event payload %zu bytes).",
        //                 //              bytes_to_post_final, total_payload_size_final);
        //                 // }
        //             }
        //             ESP_LOGI(TAG, "Finished attempt to post final partial chunk."); // LOG 14
        //         }
        //         else
        //         {
        //             ESP_LOGI(TAG, "Skipping final partial chunk post due to flags or empty buffer."); // LOG 15
        //         }

        //         // ESP_LOGI(TAG, "Reached end of do-while(false) block."); // LOG 16
        //     } while (false); // End of do-while(false) for main logic

        //     // --- Cleanup Phase ---
        //     ESP_LOGI(TAG, "Recording task entering cleanup phase."); // LOG 17

        //     ESP_LOGI(TAG, "Cleaning up chunk_event_payload_ptr.");
        //     if (chunk_event_payload_ptr)
        //     {
        //         heap_caps_free(chunk_event_payload_ptr);
        //         chunk_event_payload_ptr = nullptr;
        //         ESP_LOGI(TAG, "chunk_event_payload_ptr freed.");
        //     }
        //     else
        //     {
        //         ESP_LOGI(TAG, "chunk_event_payload_ptr was already null.");
        //     }

        //     if (self->microphone_.is_running())
        //     {
        //         ESP_LOGI(TAG, "Microphone is running. Stopping it.");
        //         self->microphone_.stop();
        //         ESP_LOGI(TAG, "Microphone stop called. Is running now: %d", (int)self->microphone_.is_running());
        //     }
        //     else
        //     {
        //         ESP_LOGI(TAG, "Microphone was not running at cleanup start.");
        //     }

        //     self->is_recording_ = false;
        //     ESP_LOGI(TAG, "Set self->is_recording_ to false. Value: %d", (int)self->is_recording_);

        //     if (self->app_event_loop_handle_ != nullptr)
        //     {
        //         ESP_LOGI(TAG, "Posting APP_EVENT_COMMAND_RECORDING_FINISHED.");
        //         // Note: Your original code posts APP_EVENT_COMMAND_RECORDING_FINISHED
        //         // but logs "APP_EVENT_COMMAND_RECORDING_STOPPED" on failure. I'll keep the event ID as FINISHED.
        //         esp_err_t ret = esp_event_post_to(self->app_event_loop_handle_,
        //                                           ADA_APP_EVENT_BASE,
        //                                           APP_EVENT_COMMAND_RECORDING_FINISHED, // Using this as per your code
        //                                           NULL,
        //                                           0,
        //                                           portMAX_DELAY);
        //         if (ret != ESP_OK)
        //         {
        //             ESP_LOGE(TAG, "Failed to post APP_EVENT_COMMAND_RECORDING_FINISHED: %s (Error refers to this event, not STOPPED)", esp_err_to_name(ret));
        //         }
        //         else
        //         {
        //             ESP_LOGI(TAG, "Posted APP_EVENT_COMMAND_RECORDING_FINISHED successfully.");
        //         }
        //     }
        //     else
        //     {
        //         ESP_LOGW(TAG, "app_event_loop_handle_ is NULL in cleanup, cannot post FINISHED event.");
        //     }

        //     ESP_LOGI(TAG, "Recording processing task finished. Preparing to delete task.");

        //     if (self->recording_task_handle_ == xTaskGetCurrentTaskHandle())
        //     {
        //         ESP_LOGI(TAG, "Clearing self->recording_task_handle_ as it matches current task.");
        //         self->recording_task_handle_ = NULL;
        //     }
        //     else if (self->recording_task_handle_ != NULL)
        //     {
        //         ESP_LOGW(TAG, "self->recording_task_handle_ (%p) does not match current task handle (%p). Not clearing it from class instance.",
        //                  self->recording_task_handle_, xTaskGetCurrentTaskHandle());
        //     }
        //     else
        //     {
        //         ESP_LOGI(TAG, "self->recording_task_handle_ was already NULL.");
        //     }

        //     ESP_LOGI(TAG, "Now self-deleting task. Goodbye!");
        //     vTaskDelete(NULL); // Self-delete
        // }

        esp_err_t AdaMicrophoneDriver::stop_recording()
        {
            if (!this->is_recording_)
            {
                ESP_LOGW(TAG, "Recording is not in progress.");
                return false;
            }

            this->microphone_.stop();
            this->is_recording_ = false;

            ESP_LOGI(TAG, "Recording stopped.");

            return true;
        }

        AdaMicrophoneDriver::~AdaMicrophoneDriver()
        {
        }
    }
}
