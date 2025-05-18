#include "esp_log.h"

#include "ada_application.hpp"

static const char *TAG = "APP_MAIN";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting Ada Smart Assistant application...");

    ada_assistant::AdaApplication ada_application;

    esp_err_t ret = ada_application.init();

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize Ada application: %s", esp_err_to_name(ret));
        // TODO Handle error
        return;
    }

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}