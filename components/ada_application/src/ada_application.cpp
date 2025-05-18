#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "ada_global_events.h"
#include "ada_application.hpp"

ESP_EVENT_DEFINE_BASE(ADA_APP_EVENT_BASE);

namespace ada_assistant
{
    static const char *TAG = "ADA_APPLICATION";

    AdaApplication::AdaApplication() : app_event_loop_handle_(nullptr),
                                       microphone_(),
                                       wake_word_engine_({
                                           .microphone = &microphone_,
                                       })

    {
    }

    esp_err_t AdaApplication::init()
    {
        ESP_LOGI(TAG, "Initializing Ada Smart Assistant application");

        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_LOGI(TAG, "Erasing NVS and re-initializing.");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        ESP_ERROR_CHECK(esp_event_loop_create_default());

        esp_event_loop_args_t app_loop_args = {
            .queue_size = 10,
            .task_name = "app_evt_loop",
            .task_priority = uxTaskPriorityGet(NULL),
            .task_stack_size = 4096,
            .task_core_id = tskNO_AFFINITY};
        ESP_ERROR_CHECK(esp_event_loop_create(&app_loop_args, &app_event_loop_handle_));

        ESP_ERROR_CHECK(esp_event_handler_register_with(app_event_loop_handle_,
                                                        ADA_APP_EVENT_BASE, ESP_EVENT_ANY_ID,
                                                        AdaApplication::app_event_handler_bridge, this));

        ESP_LOGI(TAG, "Initializing components...");

        this->microphone_.init();

        this->wake_word_engine_.init(app_event_loop_handle_);
        this->wake_word_engine_.start();

        ESP_LOGI(TAG, "Component initialization complete.");

        vTaskDelay(pdMS_TO_TICKS(100));
        esp_event_post_to(app_event_loop_handle_, ADA_APP_EVENT_BASE, ADA_DEVICE_ENABLED, NULL, 0, portMAX_DELAY);

        ESP_LOGI(TAG, "Application initialization finished. Event loop will now handle operations.");
        return ESP_OK;
    }

    void AdaApplication::app_event_handler(esp_event_base_t event_base, int32_t event_id, void *event_data)
    {
        if (event_base != ADA_APP_EVENT_BASE)
        {
            return;
        }

        switch (event_id)
        {
        case ADA_DEVICE_ENABLED:
            ESP_LOGI(TAG, "System is ready");
            break;
        case ADA_WAKE_WORD_DETECTED:
            ESP_LOGI(TAG, "Wake word detected");
            break;
        case ADA_WAKE_WORD_DETECTION_STOPPED:
            ESP_LOGI(TAG, "Wake word detection stopped");
            this->wake_word_engine_.start();
            ESP_LOGI(TAG, "Wake word detection restarted");
            break;
        default:
            ESP_LOGW(TAG, "Unknown event: %ld", event_id);
            break;
        }
    };

    void AdaApplication::app_event_handler_bridge(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data)
    {
        AdaApplication *self = static_cast<AdaApplication *>(handler_args);
        if (!self)
        {
            ESP_LOGE(TAG, "Handler args is null");
            return;
        }

        self->app_event_handler(event_base, event_id, event_data);
    }

    AdaApplication::~AdaApplication()
    {
        ESP_LOGI(TAG, "AdaApplication destructor");
    }
}
