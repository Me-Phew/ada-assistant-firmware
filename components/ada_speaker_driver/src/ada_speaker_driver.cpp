#include "esp_log.h"
#include "esp_spiffs.h"

#include "ada_speaker_driver.hpp"

namespace ada_assistant
{
    namespace speaker_driver
    {
        static const char *TAG = "AdaSpeaker";

        AdaSpeakerDriver::AdaSpeakerDriver()
            : app_event_loop_handle_(nullptr), initialized(false), playing(false), volume(100),
              i2s_stream_writer_(nullptr), mp3_decoder_(nullptr), board_handle_(nullptr),
              pipeline_(nullptr), pipeline_task_handle(nullptr), i2s_port((i2s_port_t)CONFIG_ADA_SPEAKER_DRIVER_I2S_PORT),
              sample_rate(CONFIG_ADA_SPEAKER_DRIVER_SAMPLE_RATE), bits_per_sample((i2s_data_bit_width_t)CONFIG_ADA_SPEAKER_DRIVER_BITS_PER_SAMPLE),
              file_buffer_(nullptr), file_size_(0), file_position_(0), file_loaded_(false)
        {
            ESP_LOGI(TAG, "AdaSpeakerDriver constructor");
        }

        AdaSpeakerDriver::AdaSpeakerDriver(ada_microphone_config_t config)
            : app_event_loop_handle_(nullptr), initialized(false), playing(false), volume(100),
              i2s_stream_writer_(nullptr), mp3_decoder_(nullptr), board_handle_(nullptr),
              pipeline_(nullptr), pipeline_task_handle(nullptr), i2s_port(config.i2s_port),
              sample_rate(config.sample_rate), bits_per_sample(config.bits_per_sample),
              file_buffer_(nullptr), file_size_(0), file_position_(0), file_loaded_(false)
        {
            ESP_LOGI(TAG, "AdaSpeakerDriver constructor with config");
        }

        // Update the destructor to clean up file resources
        AdaSpeakerDriver::~AdaSpeakerDriver()
        {
            ESP_LOGI(TAG, "AdaSpeakerDriver destructor");

            free_file_buffer();

            if (initialized)
            {
                deinit();
            }
        }

        audio_element_err_t AdaSpeakerDriver::file_read_callback(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx)
        {
            AdaSpeakerDriver *self = (AdaSpeakerDriver *)ctx;

            if (!self)
            {
                ESP_LOGE(TAG, "Invalid context in callback");
                return AEL_IO_FAIL;
            }

            if (!self->file_loaded_ || !self->file_buffer_)
            {
                ESP_LOGW(TAG, "No file loaded");
                return AEL_IO_FAIL;
            }

            // Check if we've reached the end of the file
            if (self->file_position_ >= self->file_size_)
            {
                ESP_LOGI(TAG, "End of file reached");
                self->playing = false;
                return AEL_IO_DONE;
            }

            // Calculate how many bytes we can read
            size_t bytes_to_read = len;
            size_t bytes_remaining = self->file_size_ - self->file_position_;

            if (bytes_to_read > bytes_remaining)
            {
                bytes_to_read = bytes_remaining;
            }

            // Copy data from memory buffer
            memcpy(buf, self->file_buffer_ + self->file_position_, bytes_to_read);
            self->file_position_ += bytes_to_read;

            ESP_LOGD(TAG, "Read %d bytes, position: %d/%d", bytes_to_read, self->file_position_, self->file_size_);

            return (audio_element_err_t)bytes_to_read;
        }

        esp_err_t AdaSpeakerDriver::load_file_to_memory(const std::string &file_path)
        {
            ESP_LOGI(TAG, "Loading file to memory: %s", file_path.c_str());

            // Free any existing buffer
            free_file_buffer();

            // Open file to get size
            FILE *file = fopen(file_path.c_str(), "rb");
            if (!file)
            {
                ESP_LOGE(TAG, "Failed to open file: %s", file_path.c_str());
                return ESP_ERR_NOT_FOUND;
            }

            // Get file size
            fseek(file, 0, SEEK_END);
            file_size_ = ftell(file);
            fseek(file, 0, SEEK_SET);

            ESP_LOGI(TAG, "File size: %d bytes", file_size_);

            // Allocate buffer
            file_buffer_ = (uint8_t *)malloc(file_size_);
            if (!file_buffer_)
            {
                ESP_LOGE(TAG, "Failed to allocate %d bytes for file buffer", file_size_);
                fclose(file);
                return ESP_ERR_NO_MEM;
            }

            // Read entire file into memory
            size_t bytes_read = fread(file_buffer_, 1, file_size_, file);
            fclose(file);

            if (bytes_read != file_size_)
            {
                ESP_LOGE(TAG, "Failed to read entire file. Read %d of %d bytes", bytes_read, file_size_);
                free_file_buffer();
                return ESP_ERR_INVALID_SIZE;
            }

            file_position_ = 0;
            file_loaded_ = true;
            current_file_path_ = file_path;

            ESP_LOGI(TAG, "File loaded successfully: %d bytes", file_size_);
            return ESP_OK;
        }

        void AdaSpeakerDriver::free_file_buffer()
        {
            if (file_buffer_)
            {
                free(file_buffer_);
                file_buffer_ = nullptr;
            }
            file_size_ = 0;
            file_position_ = 0;
            file_loaded_ = false;
        }

        void AdaSpeakerDriver::audio_pipeline_task(void *pvParameters)
        {
            ESP_LOGI(TAG, "Audio pipeline task started");
            AdaSpeakerDriver *self = (AdaSpeakerDriver *)pvParameters;

            while (1)
            {
                audio_event_iface_msg_t msg;
                esp_err_t ret = audio_event_iface_listen(self->iface_handle_, &msg, portMAX_DELAY);
                if (ret != ESP_OK)
                {
                    continue;
                }

                if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)self->mp3_decoder_ && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
                {
                    audio_element_info_t music_info = {
                        .sample_rates = 0,
                        .channels = 0,
                        .bits = 0,
                        .bps = 0,
                        .byte_pos = 0,
                        .total_bytes = 0,
                        .duration = 0,
                        .uri = NULL,
                        .codec_fmt = ESP_CODEC_TYPE_UNKNOW,
                        .reserve_data = {
                            .user_data_0 = 0,
                            .user_data_1 = 0,
                            .user_data_2 = 0,
                            .user_data_3 = 0,
                            .user_data_4 = 0,
                        },
                    };
                    audio_element_getinfo(self->mp3_decoder_, &music_info);
                    ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                             music_info.sample_rates, music_info.bits, music_info.channels);
                    i2s_stream_set_clk(self->i2s_stream_writer_, music_info.sample_rates, music_info.bits, music_info.channels);
                    continue;
                }

                if ((msg.source_type == PERIPH_ID_TOUCH || msg.source_type == PERIPH_ID_BUTTON || msg.source_type == PERIPH_ID_ADC_BTN) && (msg.cmd == PERIPH_TOUCH_TAP || msg.cmd == PERIPH_BUTTON_PRESSED || msg.cmd == PERIPH_ADC_BUTTON_PRESSED))
                {
                    if ((int)msg.data == get_input_play_id())
                    {
                        ESP_LOGI(TAG, "[ * ] [Play] touch tap event");
                        audio_element_state_t el_state = audio_element_get_state(self->i2s_stream_writer_);
                        switch (el_state)
                        {
                        case AEL_STATE_INIT:
                            ESP_LOGI(TAG, "[ * ] Starting audio pipeline");
                            audio_pipeline_run(self->pipeline_);
                            break;
                        case AEL_STATE_RUNNING:
                            ESP_LOGI(TAG, "[ * ] Pausing audio pipeline");
                            audio_pipeline_pause(self->pipeline_);
                            break;
                        case AEL_STATE_PAUSED:
                            ESP_LOGI(TAG, "[ * ] Resuming audio pipeline");
                            audio_pipeline_resume(self->pipeline_);
                            break;
                        case AEL_STATE_FINISHED:
                            ESP_LOGI(TAG, "[ * ] Rewinding audio pipeline");
                            audio_pipeline_reset_ringbuffer(self->pipeline_);
                            audio_pipeline_reset_elements(self->pipeline_);
                            audio_pipeline_change_state(self->pipeline_, AEL_STATE_INIT);
                            audio_pipeline_run(self->pipeline_);
                            break;
                        default:
                            ESP_LOGI(TAG, "[ * ] Not supported state %d", el_state);
                        }
                    }
                    else if ((int)msg.data == get_input_set_id())
                    {
                        ESP_LOGI(TAG, "[ * ] [Set] touch tap event");
                        ESP_LOGI(TAG, "[ * ] Stopping audio pipeline");
                        break;
                    }
                    else if ((int)msg.data == get_input_mode_id())
                    {
                        ESP_LOGI(TAG, "[ * ] [mode] tap event");
                        audio_pipeline_stop(self->pipeline_);
                        audio_pipeline_wait_for_stop(self->pipeline_);
                        audio_pipeline_terminate(self->pipeline_);
                        audio_pipeline_reset_ringbuffer(self->pipeline_);
                        audio_pipeline_reset_elements(self->pipeline_);
                        audio_pipeline_run(self->pipeline_);
                    }
                    else if ((int)msg.data == get_input_volup_id())
                    {
                        ESP_LOGI(TAG, "[ * ] [Vol+] touch tap event");
                        self->volume += 10;
                        if (self->volume > 100)
                        {
                            self->volume = 100;
                        }
                        audio_hal_set_volume(self->board_handle_->audio_hal, self->volume);
                        ESP_LOGI(TAG, "[ * ] Volume set to %d %%", self->volume);
                    }
                    else if ((int)msg.data == get_input_voldown_id())
                    {
                        ESP_LOGI(TAG, "[ * ] [Vol-] touch tap event");
                        self->volume -= 10;
                        if (self->volume < 0)
                        {
                            self->volume = 0;
                        }
                        audio_hal_set_volume(self->board_handle_->audio_hal, self->volume);
                        ESP_LOGI(TAG, "[ * ] Volume set to %d %%", self->volume);
                    }
                }
            }
        }

        esp_err_t AdaSpeakerDriver::init(esp_event_loop_handle_t app_event_loop_handle, int initial_volume = 100)
        {
            app_event_loop_handle_ = app_event_loop_handle;

            volume = initial_volume;

            ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
            board_handle_ = audio_board_init();
            audio_hal_ctrl_codec(board_handle_->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

            audio_hal_get_volume(board_handle_->audio_hal, &volume);

            ESP_LOGI(TAG, "[ 2 ] Create audio pipeline, add all elements to pipeline, and subscribe pipeline event");
            audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
            pipeline_ = audio_pipeline_init(&pipeline_cfg);
            mem_assert(pipeline_);

            ESP_LOGI(TAG, "[2.1] Create mp3 decoder to decode mp3 files and set custom read callback");
            mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
            mp3_decoder_ = mp3_decoder_init(&mp3_cfg);
            audio_element_set_read_cb(mp3_decoder_, file_read_callback, this);

            ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
            i2s_stream_cfg_t i2s_cfg = {
                .type = AUDIO_STREAM_WRITER,
                .transmit_mode = I2S_COMM_MODE_STD,
                .chan_cfg = {
                    .id = i2s_port,
                    .role = I2S_ROLE_MASTER,
                    .dma_desc_num = 3,
                    .dma_frame_num = 312,
                    .auto_clear = true,
                    .auto_clear_before_cb = false,
                    .intr_priority = 0,
                },
                .std_cfg = {
                    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
                    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_ADF_CONFIG(bits_per_sample, I2S_SLOT_MODE_MONO),
                    .gpio_cfg = {
                        // ? Pin definitions are not used, this is just to silence warnings
                        .mclk = GPIO_NUM_NC,
                        .bclk = GPIO_NUM_NC,
                        .ws = GPIO_NUM_NC,
                        .dout = GPIO_NUM_NC,
                        .din = GPIO_NUM_NC,
                        .invert_flags = {
                            .mclk_inv = false,
                            .bclk_inv = false,
                            .ws_inv = false,
                        },
                    },
                },
                // .pdm_rx_cfg = {
                //     .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate),
                //     .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(bits_per_sample, I2S_SLOT_MODE_MONO),
                //     .gpio_cfg = {
                //         .clk = GPIO_NUM_NC,
                //         .invert_flags = {
                //             .clk_inv = false,
                //         },
                //     },
                // },
                // .pdm_tx_cfg = {
                //     .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(sample_rate),
                //     .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(bits_per_sample, I2S_SLOT_MODE_MONO),
                //     .gpio_cfg = {
                //         .clk = GPIO_NUM_NC,
                //         .invert_flags = {
                //             .clk_inv = false,
                //         },
                //     },
                // },
                // .tdm_cfg = {
                //     .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate),
                //     .slot_cfg = {
                //         .data_bit_width = bits_per_sample,
                //         .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                //         .slot_mode = I2S_SLOT_MODE_MONO,
                //         .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
                //         .ws_width = bits_per_sample,
                //     },
                //     .gpio_cfg = {
                //         .mclk = GPIO_NUM_NC,
                //         .bclk = GPIO_NUM_NC,
                //         .ws = GPIO_NUM_NC,
                //         .invert_flags = {
                //             .mclk_inv = false,
                //             .bclk_inv = false,
                //             .ws_inv = false,
                //         },
                //     },
                // },
                .expand_src_bits = bits_per_sample,
                .use_alc = false,
                .volume = 0,
                .out_rb_size = I2S_STREAM_RINGBUFFER_SIZE,
                .task_stack = I2S_STREAM_TASK_STACK,
                .task_core = I2S_STREAM_TASK_CORE,
                .task_prio = I2S_STREAM_TASK_PRIO,
                .stack_in_ext = false,
                .multi_out_num = 0,
                .uninstall_drv = true,
                .need_expand = false,
                .buffer_len = I2S_STREAM_BUF_SIZE,
            };
            i2s_stream_writer_ = i2s_stream_init(&i2s_cfg);

            ESP_LOGI(TAG, "[2.3] Register all elements to audio pipeline");
            audio_pipeline_register(pipeline_, mp3_decoder_, "mp3");
            audio_pipeline_register(pipeline_, i2s_stream_writer_, "i2s");

            ESP_LOGI(TAG, "[2.4] Link it together [mp3_music_read_cb]-->mp3_decoder-->i2s_stream-->[codec_chip]");
            const char *link_tag[2] = {"mp3", "i2s"};
            audio_pipeline_link(pipeline_, &link_tag[0], 2);

            ESP_LOGI(TAG, "[ 3 ] Initialize peripherals");
            esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
            esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

            ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
            audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
            iface_handle_ = audio_event_iface_init(&evt_cfg);

            ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
            audio_pipeline_set_listener(pipeline_, iface_handle_);

            ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
            audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), iface_handle_);

            xTaskCreate(audio_pipeline_task, "audio_pipeline_task", 4096, this, 5, &pipeline_task_handle);

            initialized = true;

            return ESP_OK;
        }

        // Update the deinit method
        esp_err_t AdaSpeakerDriver::deinit()
        {
            ESP_LOGI(TAG, "Deinitializing AdaSpeakerDriver");

            if (!initialized)
            {
                return ESP_OK;
            }

            // Stop playback
            playing = false;

            // Free file buffer
            free_file_buffer();

            // Stop the pipeline task
            if (pipeline_task_handle)
            {
                vTaskDelete(pipeline_task_handle);
                pipeline_task_handle = nullptr;
            }

            ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
            audio_pipeline_stop(pipeline_);
            audio_pipeline_wait_for_stop(pipeline_);
            audio_pipeline_terminate(pipeline_);
            audio_pipeline_unregister(pipeline_, mp3_decoder_);
            audio_pipeline_unregister(pipeline_, i2s_stream_writer_);

            /* Terminate the pipeline before removing the listener */
            audio_pipeline_remove_listener(pipeline_);

            /* Make sure audio_pipeline_remove_listener is called before destroying event_iface */
            audio_event_iface_destroy(iface_handle_);

            /* Release all resources */
            audio_pipeline_deinit(pipeline_);
            audio_element_deinit(i2s_stream_writer_);
            audio_element_deinit(mp3_decoder_);

            initialized = false;

            return ESP_OK;
        }

        esp_err_t AdaSpeakerDriver::play_mp3_file(std::string file_path)
        {
            ESP_LOGI(TAG, "Playing mp3 file: %s", file_path.c_str());

            if (!initialized)
            {
                ESP_LOGE(TAG, "Speaker driver not initialized");
                return ESP_ERR_INVALID_STATE;
            }

            // Stop current playback if running
            if (playing)
            {
                stop();
            }

            // Load file into memory
            esp_err_t ret = load_file_to_memory(file_path);
            if (ret != ESP_OK)
            {
                return ret;
            }

            // Reset and start the pipeline
            audio_pipeline_stop(pipeline_);
            audio_pipeline_wait_for_stop(pipeline_);
            audio_pipeline_reset_ringbuffer(pipeline_);
            audio_pipeline_reset_elements(pipeline_);
            audio_pipeline_change_state(pipeline_, AEL_STATE_INIT);

            ret = audio_pipeline_run(pipeline_);
            if (ret == ESP_OK)
            {
                playing = true;
                ESP_LOGI(TAG, "Started playing: %s", file_path.c_str());
            }
            else
            {
                ESP_LOGE(TAG, "Failed to start pipeline: %s", esp_err_to_name(ret));
                free_file_buffer();
            }

            return ret;
        }

        esp_err_t AdaSpeakerDriver::play_mp3_file_blocking(std::string file_path)
        {
            ESP_LOGI(TAG, "Playing mp3 file blocking: %s", file_path.c_str());

            esp_err_t ret = play_mp3_file(file_path);
            if (ret != ESP_OK)
            {
                return ret;
            }

            // Wait for playback to complete
            audio_event_iface_msg_t msg;
            while (playing)
            {
                ret = audio_event_iface_listen(iface_handle_, &msg, 1000 / portTICK_PERIOD_MS);
                if (ret != ESP_OK)
                {
                    continue;
                }

                // Check for end of stream or error
                if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT)
                {
                    if (msg.source == (void *)mp3_decoder_)
                    {
                        if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
                        {
                            audio_element_state_t el_state = audio_element_get_state(mp3_decoder_);
                            if (el_state == AEL_STATE_FINISHED || el_state == AEL_STATE_ERROR)
                            {
                                ESP_LOGI(TAG, "Playback finished");
                                playing = false;
                                break;
                            }
                        }
                    }
                }
            }

            // Clean up
            free_file_buffer();

            return ESP_OK;
        }

        esp_err_t AdaSpeakerDriver::pause()
        {
            ESP_LOGI(TAG, "Pausing audio pipeline");
            return audio_pipeline_pause(pipeline_);
        }

        // Update stop method to handle file cleanup
        esp_err_t AdaSpeakerDriver::stop()
        {
            ESP_LOGI(TAG, "Stopping audio pipeline");

            playing = false;

            audio_pipeline_stop(pipeline_);
            esp_err_t ret = audio_pipeline_wait_for_stop(pipeline_);

            // Free file buffer
            free_file_buffer();

            return ret;
        }

        esp_err_t AdaSpeakerDriver::resume()
        {
            ESP_LOGI(TAG, "Resuming audio pipeline");
            return audio_pipeline_resume(pipeline_);
        }

        esp_err_t AdaSpeakerDriver::set_volume(int newVolume)
        {
            if (newVolume < 0 || newVolume > 100)
            {
                ESP_LOGE(TAG, "Volume must be between 0 and 100");
                return ESP_ERR_INVALID_ARG;
            }
            volume = newVolume;
            audio_hal_set_volume(board_handle_->audio_hal, newVolume);
            ESP_LOGI(TAG, "Volume set to %d %%", newVolume);
            return ESP_OK;
        }

        int AdaSpeakerDriver::get_volume()
        {
            return volume;
        }

        bool AdaSpeakerDriver::is_running()
        {
            if (!initialized)
            {
                return false;
            }

            audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer_);
            return (el_state == AEL_STATE_RUNNING) && playing;
        }

        bool AdaSpeakerDriver::is_stopped()
        {
            if (!initialized)
            {
                return true;
            }

            audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer_);
            return (el_state != AEL_STATE_RUNNING) || !playing;
        }
    }
}