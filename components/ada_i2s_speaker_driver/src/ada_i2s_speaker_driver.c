#include "string.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef CONFIG_ADA_I2S_SPEAKER_ENABLE_POTENTIOMETER_VOLUME_CONTROL_YES
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

#include "ada_i2s_speaker_driver.h"
#include "ada_i2s_speaker_driver_pinout.h"

static const char *TAG = "ADA_I2S_SPEAKER_DRIVER";

#define I2S_NUM I2S_NUM_AUTO

#define SAMPLE_RATE 44100
#define BITS_PER_CHAN I2S_DATA_BIT_WIDTH_16BIT
#define CHANNEL_FORMAT I2S_SLOT_MODE_MONO

#define CHUNK_SIZE 4096 // Size of buffer to read from file and send to I2S

static TaskHandle_t playback_task_handle = NULL;
static volatile bool stop_playback_flag = false;
static SemaphoreHandle_t i2s_mutex = NULL;

static audio_playback_finished_cb_t playback_finished_cb = NULL;

// Simple state tracking to prevent double init/deinit
static bool g_i2s_speaker_initialized = false;

static i2s_chan_handle_t tx_handle = NULL; // I2S rx channel handler

#ifdef CONFIG_ADA_I2S_SPEAKER_ENABLE_POTENTIOMETER_VOLUME_CONTROL_YES

static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t adc_cali_handle = NULL;
static bool cali_enable = false;

// Default configuration
#define VOLUME_MIN 0
#define VOLUME_MAX 100
#define SAMPLES 10 // For averaging multiple readings

// Function to map GPIO pin to ADC channel for ESP32-S3
static int gpio_to_adc_channel(int gpio_num)
{
    // ESP32-S3 specific mapping
    switch (gpio_num)
    {
    case 1:
        return 0; // ADC1_CHANNEL_0
    case 2:
        return 1; // ADC1_CHANNEL_1
    case 3:
        return 2; // ADC1_CHANNEL_2
    case 4:
        return 3; // ADC1_CHANNEL_3
    case 5:
        return 4; // ADC1_CHANNEL_4
    case 6:
        return 5; // ADC1_CHANNEL_5
    case 7:
        return 6; // ADC1_CHANNEL_6
    case 8:
        return 7; // ADC1_CHANNEL_7
    case 9:
        return 8; // ADC1_CHANNEL_8
    case 10:
        return 9; // ADC1_CHANNEL_9
    default:
        return 9; // Default to GPIO10/ADC1_CH9
    }
}

/**
 * @brief Initialize the ADC for potentiometer reading (ESP-IDF 5.0+ compatible)
 */
esp_err_t speaker_potentiometer_init(void)
{
    // ADC initialization configuration
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };

    // Initialize ADC
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    // Get the ADC channel from the configured GPIO pin
    int adc_channel = gpio_to_adc_channel(CONFIG_ADA_I2S_SPEAKER_POTENTIOMETER_GPIO);

    // ADC configuration
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };

    // Configure the ADC channel
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, adc_channel, &config));

    // ADC calibration configuration
    adc_cali_handle_t cali_handle = NULL;

#ifdef ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    // ESP32, ESP32S2, ESP32S3 support curve fitting scheme
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));
    cali_enable = true;
    adc_cali_handle = cali_handle;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    // ESP32C3, ESP32H2, ESP32C2 support line fitting scheme
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle));
    cali_enable = true;
    adc_cali_handle = cali_handle;
#else
    ESP_LOGW(TAG, "No calibration scheme supported, raw ADC results will be used");
#endif

    ESP_LOGI(TAG, "Potentiometer ADC initialized on GPIO pin %d", CONFIG_ADA_I2S_SPEAKER_POTENTIOMETER_GPIO);

    return ESP_OK;
}

/**
 * @brief Read volume level from potentiometer (ESP-IDF 5.0+ compatible)
 *
 * @return uint8_t Volume level from VOLUME_MIN to VOLUME_MAX
 */
uint8_t speaker_potentiometer_volume_read(void)
{
    // Get the ADC channel from the configured GPIO pin
    int adc_channel = gpio_to_adc_channel(CONFIG_ADA_I2S_SPEAKER_POTENTIOMETER_GPIO);

    int adc_reading = 0;

    // Take multiple samples and average them
    for (int i = 0; i < SAMPLES; i++)
    {
        int raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, adc_channel, &raw));
        adc_reading += raw;
    }
    adc_reading /= SAMPLES;

    int voltage = 0;

    // Convert ADC reading to voltage in mV if calibration is enabled
    if (cali_enable)
    {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, adc_reading, &voltage));
    }
    else
    {
        // Approximate conversion if no calibration
        voltage = adc_reading * 3300 / 4095; // For 12-bit ADC
    }

    // Convert voltage to volume level (0-100)
    uint8_t volume = (uint8_t)((voltage * (VOLUME_MAX - VOLUME_MIN)) / 3300);

    // Ensure volume is within range
    if (volume > VOLUME_MAX)
    {
        volume = VOLUME_MAX;
    }

    ESP_LOGD(TAG, "ADC Reading: %d, Voltage: %dmV, Volume: %d%%", adc_reading, voltage, volume);

    return volume;
}

/**
 * @brief Clean up ADC resources (call this on shutdown)
 */
void speaker_potentiometer_deinit(void)
{
    if (cali_enable)
    {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(adc_cali_handle));
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(adc_cali_handle));
#endif
    }

    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc_handle));
    ESP_LOGI(TAG, "Potentiometer ADC deinitialized");
}

#endif

esp_err_t ada_i2s_speaker_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S speaker driver...");
    esp_err_t ret_val = ESP_OK;

    if (g_i2s_speaker_initialized)
    {
        ESP_LOGW(TAG, "I2S speaker already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (i2s_mutex == NULL)
    {
        i2s_mutex = xSemaphoreCreateMutex();
    }

#ifdef CONFIG_ADA_I2S_SPEAKER_ENABLE_POTENTIOMETER_VOLUME_CONTROL_YES

    ret_val = speaker_potentiometer_init();
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize potentiometer ADC: %s", esp_err_to_name(ret_val));
        return ret_val;
    }
    ESP_LOGI(TAG, "Potentiometer ADC initialized successfully");

#endif

    ESP_LOGI(TAG, "Configuring I2S channel with sample rate: %d, channel format: %d, bits per channel: %d",
             SAMPLE_RATE, CHANNEL_FORMAT, BITS_PER_CHAN);
    ESP_LOGI(TAG, "I2S GPIO pins: LRCK=GPIO_NUM_%d, BCLK=GPIO_NUM_%d, SDOUT=GPIO_NUM_%d",
             GPIO_I2S_LRCK, GPIO_I2S_BCLK, GPIO_I2S_SDOUT);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);

    ESP_LOGD(TAG, "Creating new I2S channel");
    ret_val = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret_val));
        return ret_val;
    }

    i2s_std_config_t std_cfg = I2S_CONFIG_DEFAULT(SAMPLE_RATE, BITS_PER_CHAN, CHANNEL_FORMAT);

    ESP_LOGD(TAG, "Initializing I2S channel in STD mode");
    ret_val = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize I2S STD mode: %s", esp_err_to_name(ret_val));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return ret_val;
    }

    g_i2s_speaker_initialized = true;
    ESP_LOGI(TAG, "I2S speaker driver initialization complete");

    return ret_val;
}

esp_err_t enable_i2s_channel(void)
{
    esp_err_t ret_val = ESP_OK;

    if (tx_handle == NULL)
    {
        ESP_LOGE(TAG, "Invalid tx_handle (NULL) during enable operation");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Enabling I2S channel");
    ret_val = i2s_channel_enable(tx_handle);
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret_val));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return ret_val;
    }

    return ret_val;
}

esp_err_t disable_i2s_channel(void)
{
    esp_err_t ret_val = ESP_OK;

    if (tx_handle == NULL)
    {
        ESP_LOGE(TAG, "Invalid tx_handle (NULL) during disable operation");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Disabling I2S channel");
    ret_val = i2s_channel_disable(tx_handle);
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(ret_val));
        return ret_val;
    }

    return ret_val;
}

esp_err_t ada_i2s_speaker_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing I2S speaker driver");
    esp_err_t ret_val = ESP_OK;

    if (!g_i2s_speaker_initialized)
    {
        ESP_LOGW(TAG, "I2S speaker not initialized, nothing to deinitialize");
        return ESP_ERR_INVALID_STATE;
    }

    if (tx_handle == NULL)
    {
        ESP_LOGE(TAG, "Invalid tx_handle (NULL) during deinitialization");
        g_i2s_speaker_initialized = false;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Disabling I2S channel");
    ret_val = disable_i2s_channel();
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(ret_val));
        return ret_val;
    }

    ESP_LOGD(TAG, "Deleting I2S channel");
    esp_err_t del_ret = i2s_del_channel(tx_handle);
    if (del_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to delete I2S channel: %s", esp_err_to_name(del_ret));
        ret_val |= del_ret;
    }

#ifdef CONFIG_ADA_I2S_SPEAKER_ENABLE_POTENTIOMETER_VOLUME_CONTROL_YES

    speaker_potentiometer_deinit();
    ESP_LOGI(TAG, "Potentiometer ADC deinitialized successfully");

#endif

    tx_handle = NULL;
    g_i2s_speaker_initialized = false;
    ESP_LOGI(TAG, "I2S speaker driver deinitialization %s", (ret_val == ESP_OK) ? "successful" : "failed");

    return ret_val;
}

esp_err_t ada_i2s_speaker_write(const void *src, size_t size, size_t *bytes_written, TickType_t ticks_to_wait)
{
    // Fixed format specifier: Use PRIu32 for TickType_t (long unsigned int)
    ESP_LOGV(TAG, "Writing to I2S speaker: src=%p, size=%u, timeout=%" PRIu32 " ticks",
             src, size, (uint32_t)ticks_to_wait);

    if (src == NULL || size == 0 || bytes_written == NULL)
    {
        ESP_LOGE(TAG, "Invalid arguments for i2s_speaker_write (src=%p, size=%u, bytes_written=%p)",
                 src, size, bytes_written);
        if (bytes_written)
        {
            *bytes_written = 0; // Ensure bytes_written is 0 on error
        }

        return ESP_ERR_INVALID_ARG;
    }

    if (!g_i2s_speaker_initialized)
    {
        ESP_LOGE(TAG, "I2S channel not initialized");
        *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }

    if (tx_handle == NULL)
    {
        ESP_LOGE(TAG, "Invalid tx_handle (NULL) during write operation");
        *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Calling i2s_channel_write with size %u bytes", size);

#ifdef CONFIG_ADA_I2S_SPEAKER_ENABLE_POTENTIOMETER_VOLUME_CONTROL_YES
    uint8_t volume = speaker_potentiometer_volume_read();

    if (volume == 0)
    {
        ESP_LOGW(TAG, "Volume is 0, not writing to I2S channel");
        *bytes_written = 0;
        return ESP_OK; // No data to write
    }

    // Make a scaled copy of the audio data based on volume level
    int16_t *scaled_buffer = NULL;
    if (volume < 100)
    { // Only scale if volume is less than 100%
        scaled_buffer = malloc(size);
        if (scaled_buffer == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for volume scaling");
            *bytes_written = 0;
            return ESP_ERR_NO_MEM;
        }

        // Scale each 16-bit sample by the volume percentage
        const int16_t *src_16bit = (const int16_t *)src;
        size_t sample_count = size / sizeof(int16_t);

        for (size_t i = 0; i < sample_count; i++)
        {
            scaled_buffer[i] = (int16_t)((src_16bit[i] * volume) / 100);
        }

        // Use the scaled buffer instead of the original source
        src = scaled_buffer;
    }

    // The original i2s_channel_write will be called after this code block
    // After the write, free the scaled buffer if it was allocated
    esp_err_t ret = i2s_channel_write(tx_handle, src, size, bytes_written, ticks_to_wait);

    // Clean up the scaled buffer
    if (scaled_buffer != NULL)
    {
        free(scaled_buffer);
    }

#else
    esp_err_t ret = i2s_channel_write(tx_handle, src, size, bytes_written, ticks_to_wait);

#endif

    if (ret != ESP_OK)
    {
        // Don't log ESP_ERR_TIMEOUT as an error necessarily, it's an expected condition
        if (ret == ESP_ERR_TIMEOUT)
        {
            ESP_LOGW(TAG, "I2S write timeout: requested=%u bytes, written=%u bytes", size, *bytes_written);
        }
        else
        {
            ESP_LOGE(TAG, "I2S write error: %s, requested=%u bytes, written=%u bytes",
                     esp_err_to_name(ret), size, *bytes_written);
        }
    }
    else
    {
        ESP_LOGD(TAG, "I2S write successful: requested=%u bytes, written=%u bytes", size, *bytes_written);
    }

    return ret;
}

static void audio_playback_task(void *arg)
{
    const char *filename = (const char *)arg;
    FILE *file = fopen(filename, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Playing PCM file: %s", filename);

    // Buffer for reading data
    uint8_t *buffer = malloc(CHUNK_SIZE);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for audio buffer");
        fclose(file);
        vTaskDelete(NULL);
        return;
    }

    // Read and play chunks of data
    size_t bytes_read = 0;
    size_t bytes_written = 0;
    esp_err_t ret = ESP_OK;

    if (!xSemaphoreTake(i2s_mutex, portMAX_DELAY))
    {
        ESP_LOGE(TAG, "Failed to aquire I2S mutex");
        free(buffer);
        fclose(file);
        return;
    }

    enable_i2s_channel();

    while (!stop_playback_flag && (bytes_read = fread(buffer, 1, CHUNK_SIZE, file)) > 0)
    {
        ret = ada_i2s_speaker_write(buffer, bytes_read, &bytes_written, 1000);
        if (ret != ESP_OK || bytes_written != bytes_read)
        {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            break;
        }
    }

    // Clean up
    disable_i2s_channel();
    xSemaphoreGive(i2s_mutex);
    free(buffer);
    fclose(file);

    ESP_LOGI(TAG, "Playback finished or stopped");

    playback_task_handle = NULL; // Clear task handle

    if (playback_finished_cb)
    {
        playback_finished_cb();
    }

    vTaskDelete(NULL);
}

void set_audio_playback_finished_callback(audio_playback_finished_cb_t cb)
{
    playback_finished_cb = cb;
}

esp_err_t ada_i2s_start_file_playback(const char *filename)
{
    if (playback_task_handle != NULL)
    {
        ESP_LOGW(TAG, "Audio is already playing");
        return ESP_ERR_INVALID_STATE;
    }

    stop_playback_flag = false;

    // Use strdup so filename is valid when task starts
    char *filename_copy = strdup(filename);
    if (!filename_copy)
    {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(audio_playback_task, "audio_playback_task", 4096 * 2, filename_copy, 5, &playback_task_handle);

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create playback task");
        free(filename_copy);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ada_i2s_stop_playback(void)
{
    if (playback_task_handle == NULL)
    {
        ESP_LOGW(TAG, "No playback task to stop");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping playback...");
    stop_playback_flag = true;

    // Optionally wait for the task to end (synchronous stop)
    while (playback_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Playback stopped");
    return ESP_OK;
}
