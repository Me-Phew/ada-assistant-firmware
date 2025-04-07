#include "esp_log.h"

#include "ada_led_strip_driver.h"
#include "ada_wake_word_detection_engine.h"
#include "ada_i2s_speaker_driver.h"
#include "utils.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    configure_led_strip();

    ESP_LOGI(TAG, "Starting main application...");

    esp_err_t ret;

    ret = mountSPIFFS("/audio", "audio", 5);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return; // Cannot continue without SPIFFS
    }

    ret = configure_led_strip();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure LED strip: %s", esp_err_to_name(ret));
        return; // Cannot continue without LED strip
    }

    ret = ada_i2s_speaker_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize I2S speaker driver: %s", esp_err_to_name(ret));
        return; // Cannot continue
    }

    ada_i2s_start_file_playback("/audio/windows_7_startup.pcm");

    init_wake_word_detection_engine();

    ESP_LOGI(TAG, "Example finished.");
}
