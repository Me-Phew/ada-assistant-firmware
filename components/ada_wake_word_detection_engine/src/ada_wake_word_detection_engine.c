#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "model_path.h"
#include "string.h"
#include "esp_timer.h"

#include "ada_i2s_microphone_driver.h"
#include "ada_i2s_speaker_driver.h"
#include "ada_led_strip_driver.h"

#include "ada_wake_word_detection_engine.h"

static const char *TAG = "ADA_WAKE_WORD_DETECTION_ENGINE";

static esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static volatile int task_flag = 0;
static volatile bool pause_feed_task = false;

typedef enum
{
    STATE_DETECTING, // Listening for wake word
    STATE_RECORDING  // Recording user speech after wake word
} detection_state_t;

static volatile detection_state_t current_state = STATE_DETECTING;
static int16_t *recording_buffer = NULL;
static size_t recording_length = 0;
static size_t max_recording_length = 0;
#define SILENCE_THRESHOLD 1000          // Adjust based on your noise floor
#define SILENCE_DURATION_MS 1500        // Stop recording after 1.5 seconds of silence
#define MIN_RECORDING_DURATION_MS 2000  // Minimum recording time (2 seconds)
#define MAX_RECORDING_DURATION_MS 10000 // Maximum recording time (10 seconds)

static void process_recording(void)
{
    // Process the recorded audio data in recording_buffer
    ESP_LOGI(TAG, "Processing recorded audio...\n");

    ada_led_strip_stop_effect();
    clear_led_strip();
    // ada_led_strip_start_sequential_fade_in_with_duration(0, 255, 80, 0, 5000, false);
    ada_led_strip_start_color_breathing_with_duration(0, 255, 80, 0, 5000, 3);

    ada_i2s_start_file_playback("/audio/custom_listening_end.pcm");

    vTaskDelay(pdMS_TO_TICKS(5000));

    ada_led_strip_stop_effect();
    clear_led_strip();

    ada_led_strip_start_color_breathing_with_duration(0, 255, 0, 0, 5000, 6);
    ada_i2s_start_file_playback("/audio/error_lost_wifi_connection.pcm");

    vTaskDelay(pdMS_TO_TICKS(5000));

    ada_led_strip_stop_effect();
    clear_led_strip();

    ada_i2s_start_file_playback("/audio/lounge_act.pcm");
    ada_led_strip_start_color_breathing_with_duration(0, 0, 255, 125, 38000, 19);

    // Reset recording buffer and length
    memset(recording_buffer, 0, max_recording_length);
    recording_length = 0;
}

void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int nch = afe_handle->get_feed_channel_num(afe_data);
    int feed_channel = i2s_microphone_get_feed_channel();
    assert(nch == feed_channel);
    int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
    assert(i2s_buff);

    while (task_flag)
    {
        if (!pause_feed_task)
        {
            // Feed audio to AFE only when not paused
            i2s_microphone_get_feed_data(i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);
            afe_handle->feed(afe_data, i2s_buff);
        }
        else
        {
            // When paused, just add a small delay to prevent CPU hogging
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (i2s_buff)
    {
        free(i2s_buff);
        i2s_buff = NULL;
    }
    vTaskDelete(NULL);
}

// Task created only for recording and then terminates
void record_audio_task(void *arg)
{
    ESP_LOGI(TAG, "-----------RECORDING USER SPEECH-----------\n");

    // clear leds
    ada_led_strip_stop_effect();
    clear_led_strip();

    ada_led_strip_start_sequential_fade_in_with_duration(0, 0, 0, 100, 500, false);

    // clear speaker
    ada_i2s_stop_playback();

    ada_i2s_start_file_playback("/audio/custom_listening_start.pcm");

    // wait for the animation and playback
    vTaskDelay(pdMS_TO_TICKS(500));

    ada_led_strip_stop_effect();
    ada_led_strip_start_sequential_fade_out_with_duration(CONFIG_ADA_LED_STRIP_MAX_LEDS - 1, 0, 0, 100, MAX_RECORDING_DURATION_MS, true);

    int sample_rate = afe_handle->get_samp_rate(afe_data);
    int samples_per_ms = sample_rate / 1000;
    int silence_duration_samples = samples_per_ms * SILENCE_DURATION_MS;
    int direct_chunksize = 320; // Adjust based on your requirements
    int feed_channel = i2s_microphone_get_feed_channel();

    int16_t *direct_buff = malloc(direct_chunksize * sizeof(int16_t) * feed_channel);
    assert(direct_buff);

    uint32_t recording_start_time = esp_timer_get_time() / 1000; // Convert to ms
    recording_length = 0;
    int silence_counter = 0;
    bool recording_complete = false;

    while (!recording_complete && task_flag)
    {
        // Get data directly from microphone
        i2s_microphone_get_feed_data(direct_buff, direct_chunksize * sizeof(int16_t) * feed_channel);

        // Process only the first channel if multi-channel
        int16_t *mono_data = malloc(direct_chunksize * sizeof(int16_t));
        for (int i = 0; i < direct_chunksize; i++)
        {
            mono_data[i] = direct_buff[i * feed_channel]; // Extract first channel
        }

        if (recording_length + direct_chunksize * sizeof(int16_t) <= max_recording_length)
        {
            // Copy audio data to recording buffer
            memcpy(recording_buffer + recording_length / sizeof(int16_t),
                   mono_data,
                   direct_chunksize * sizeof(int16_t));
            recording_length += direct_chunksize * sizeof(int16_t);

            // Check for silence
            bool is_silent = true;
            for (int i = 0; i < direct_chunksize; i++)
            {
                if (abs(mono_data[i]) > SILENCE_THRESHOLD)
                {
                    is_silent = false;
                    ESP_LOGI(TAG, "Noise detected: %d\n", abs(mono_data[i]) - SILENCE_THRESHOLD);
                    break;
                }
            }

            uint32_t current_time = esp_timer_get_time() / 1000;
            uint32_t recording_duration = current_time - recording_start_time;

            if (is_silent)
            {
                silence_counter += direct_chunksize;
                if (silence_counter >= silence_duration_samples && recording_duration > MIN_RECORDING_DURATION_MS)
                {
                    // Silence detected for the duration threshold
                    recording_complete = true;
                }
            }
            else
            {
                silence_counter = 0; // Reset silence counter
            }

            if (recording_duration >= MAX_RECORDING_DURATION_MS)
            {
                ESP_LOGI(TAG, "Maximum recording duration reached");
                recording_complete = true;
            }
        }
        else
        {
            // Buffer full
            ESP_LOGI(TAG, "Recording buffer full");
            recording_complete = true;
        }

        free(mono_data);
    }

    process_recording();

    if (direct_buff)
    {
        free(direct_buff);
    }

    // Signal that we're done recording
    current_state = STATE_DETECTING;
    pause_feed_task = false;
    ESP_LOGI(TAG, "-----------RESUMING DETECTION-----------\n");

    vTaskDelete(NULL);
}

void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    int16_t *buff = malloc(afe_chunksize * sizeof(int16_t));
    assert(buff);

    // Configure recording buffer
    max_recording_length = afe_handle->get_samp_rate(afe_data) * MAX_RECORDING_DURATION_MS / 1000 * sizeof(int16_t);
    recording_buffer = malloc(max_recording_length);
    assert(recording_buffer);

    ESP_LOGI(TAG, "------------detect start------------\n");

    while (task_flag)
    {
        // Only fetch from AFE when in detecting state
        if (current_state == STATE_DETECTING)
        {
            afe_fetch_result_t *res = afe_handle->fetch(afe_data);
            if (!res || res->ret_value == ESP_FAIL)
            {
                ESP_LOGE(TAG, "fetch error!\n");
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Check for wake word
            if (res->wakeup_state == WAKENET_DETECTED)
            {
                ESP_LOGI(TAG, "wakeword detected\n");
                ESP_LOGI(TAG, "model index:%d, word index:%d\n", res->wakenet_model_index, res->wake_word_index);

                // Switch to recording state
                current_state = STATE_RECORDING;

                // Pause the feed task to stop feeding data to AFE
                pause_feed_task = true;

                // Create a recording task that will handle the recording
                TaskHandle_t record_task_handle;
                xTaskCreatePinnedToCore(&record_audio_task, "record", 8 * 1024, NULL, 5, &record_task_handle, 0);
            }
        }
        else
        {
            // When in recording state, just add a small delay to prevent CPU hogging
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (buff)
    {
        free(buff);
        buff = NULL;
    }

    if (recording_buffer)
    {
        free(recording_buffer);
        recording_buffer = NULL;
    }

    vTaskDelete(NULL);
}

void init_wake_word_detection_engine()
{
    ESP_ERROR_CHECK(i2s_microphone_init());

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models)
    {
        for (int i = 0; i < models->num; i++)
        {
            if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL)
            {
                ESP_LOGI(TAG, "wakenet model in flash: %s\n", models->model_name[i]);
            }
        }
    }

    afe_config_t *afe_config = afe_config_init(i2s_microphone_get_input_format(), models, AFE_TYPE_SR, AFE_MODE_LOW_COST);

    if (afe_config->wakenet_model_name)
    {
        ESP_LOGI(TAG, "wakeword model in AFE config: %s\n", afe_config->wakenet_model_name);
    }

    if (afe_config->wakenet_model_name_2)
    {
        ESP_LOGI(TAG, "wakeword model in AFE config: %s\n", afe_config->wakenet_model_name_2);
    }

    afe_handle = esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    task_flag = 1;
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void *)afe_data, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 4 * 1024, (void *)afe_data, 5, NULL, 1);
}
