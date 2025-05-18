#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

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
                                       }),
                                       soft_enable_button_gpio(static_cast<gpio_num_t>(CONFIG_ADA_APP_EN_BTN_GPIO)),
                                       soft_enable_button_active_level(CONFIG_ADA_APP_EN_BTN_ACTIVE_LEVEL),
                                       soft_enable_button_debounce_time_ms(CONFIG_ADA_APP_EN_BTN_DEBOUNCE_TIME_MS),
                                       soft_enable_button_polling_rate_ms(CONFIG_ADA_APP_EN_BTN_POLLING_RATE_MS),
                                       status_led_gpio(static_cast<gpio_num_t>(CONFIG_ADA_APP_EN_LED_GPIO)),
                                       is_shutdown_requested(false)

    {
    }

    esp_err_t AdaApplication::init_soft_enable_button()
    {
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << soft_enable_button_gpio);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

        ESP_ERROR_CHECK(gpio_config(&io_conf));

        return ESP_OK;
    }

    bool AdaApplication::is_soft_enable_button_on()
    {
        int level = gpio_get_level(soft_enable_button_gpio);

        return (level == soft_enable_button_active_level);
    }

    esp_err_t AdaApplication::init_nvs()
    {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_LOGI(TAG, "Erasing NVS and re-initializing.");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }

        ESP_ERROR_CHECK(ret);

        return ret;
    }

    esp_err_t AdaApplication::init_event_loop()
    {
        esp_err_t ret = esp_event_loop_create_default();
        ESP_ERROR_CHECK(ret);

        esp_event_loop_args_t app_loop_args = {
            .queue_size = 10,
            .task_name = "app_event_loop",
            .task_priority = uxTaskPriorityGet(NULL),
            .task_stack_size = 4096,
            .task_core_id = tskNO_AFFINITY};

        ret = esp_event_loop_create(&app_loop_args, &app_event_loop_handle_);
        ESP_ERROR_CHECK(ret);

        ret = esp_event_handler_register_with(app_event_loop_handle_,
                                              ADA_APP_EVENT_BASE, ESP_EVENT_ANY_ID,
                                              AdaApplication::app_event_handler_bridge, this);

        ESP_ERROR_CHECK(ret);

        return ret;
    }

    esp_err_t AdaApplication::init_status_led()
    {
        ESP_LOGI(TAG, "Initializing status LED on GPIO %d", status_led_gpio);

        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << status_led_gpio);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

        esp_err_t ret = gpio_config(&io_conf);
        ESP_ERROR_CHECK(ret);

        return ret;
    }

    esp_err_t AdaApplication::set_status_led_state(bool on)
    {
        esp_err_t ret = gpio_set_level(status_led_gpio, on ? 1 : 0);
        ESP_ERROR_CHECK(ret);

        return ret;
    }

    esp_err_t AdaApplication::init_components()
    {
        ESP_LOGI(TAG, "Initializing components...");

        this->microphone_.init();

        this->wake_word_engine_.init(app_event_loop_handle_);
        this->wake_word_engine_.start();

        ESP_LOGI(TAG, "Component initialization complete.");

        return ESP_OK;
    }

    esp_err_t AdaApplication::request_shutdown()
    {
        ESP_LOGI(TAG, "Requesting shutdown...");

        esp_event_post_to(app_event_loop_handle_, ADA_APP_EVENT_BASE, ADA_DEVICE_SHUTDOWN_REQUESTED, NULL, 0, portMAX_DELAY);
        this->is_shutdown_requested = true;

        return ESP_OK;
    }

    esp_err_t AdaApplication::init()
    {
        ESP_LOGI(TAG, "Initializing Ada Smart Assistant application");

        esp_err_t ret = this->init_status_led();
        ESP_ERROR_CHECK(ret);

        ret = this->init_soft_enable_button();
        ESP_ERROR_CHECK(ret);

        if (!is_soft_enable_button_on())
        {
            ESP_LOGI(TAG, "Soft enable button is off. Shutting down.");
            this->is_shutdown_requested = true;

            set_status_led_state(false);

            return ESP_OK;
        }

        ESP_LOGI(TAG, "Soft enable button is on. Device is in startup.");

        ret = this->init_nvs();
        ESP_ERROR_CHECK(ret);

        ret = this->init_event_loop();
        ESP_ERROR_CHECK(ret);

        ret = this->init_components();
        ESP_ERROR_CHECK(ret);

        ESP_LOGI(TAG, "Application initialization finished. Handing off control to the event loop.");

        vTaskDelay(pdMS_TO_TICKS(100));
        esp_event_post_to(app_event_loop_handle_, ADA_APP_EVENT_BASE, ADA_DEVICE_READY, NULL, 0, portMAX_DELAY);

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
        case ADA_DEVICE_READY:
            ESP_LOGI(TAG, "Main loop control handoff successful");
            set_status_led_state(true);
            break;
        case ADA_WAKE_WORD_DETECTED:
            ESP_LOGI(TAG, "Wake word detected");
            break;
        case ADA_WAKE_WORD_DETECTION_STOPPED:
            ESP_LOGI(TAG, "Wake word detection stopped");

            if (this->is_shutdown_requested)
            {
                break;
            }

            this->wake_word_engine_.start();
            ESP_LOGI(TAG, "Wake word detection restarted");
            break;
        default:
            ESP_LOGW(TAG, "Not handling unknown event: %ld", event_id);
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

    void AdaApplication::run_shutdown_listener()
    {
        ESP_LOGI(TAG, "Running until shutdown requested");

        while (!this->is_shutdown_requested)
        {
            if (!this->is_soft_enable_button_on())
            {
                vTaskDelay(pdMS_TO_TICKS(soft_enable_button_debounce_time_ms));

                if (!this->is_soft_enable_button_on())
                {
                    ESP_LOGI(TAG, "Soft enable button is off. Shutting down.");
                    this->is_shutdown_requested = true;

                    break;
                }

                ESP_LOGI(TAG, "Soft enable button was off, but it is on after %d ms debounce. Ignoring.", soft_enable_button_debounce_time_ms);
            }

            vTaskDelay(pdMS_TO_TICKS(soft_enable_button_polling_rate_ms));
        }

        this->shutdown();
    }

    esp_err_t AdaApplication::deinit_components()
    {
        ESP_LOGI(TAG, "Deinitializing components...");

        if (this->wake_word_engine_.wakeWord_.is_running())
        {
            this->wake_word_engine_.stop();
        }

        if (this->microphone_.is_running())
        {
            this->microphone_.stop();
        }

        ESP_LOGI(TAG, "Component deinitialization complete.");

        return ESP_OK;
    }

    esp_err_t AdaApplication::deinit_event_loop()
    {
        ESP_LOGI(TAG, "Deinitializing event loop...");

        if (app_event_loop_handle_)
        {
            return esp_event_handler_unregister_with(app_event_loop_handle_, ADA_APP_EVENT_BASE, ESP_EVENT_ANY_ID, AdaApplication::app_event_handler_bridge);
        }

        return ESP_OK;
    }

    esp_err_t AdaApplication::configure_wakeup_source()
    {
        ESP_LOGI(TAG, "Configuring wakeup source on GPIO %d (%d level for ON)", soft_enable_button_gpio, soft_enable_button_active_level);

        if (!rtc_gpio_is_valid_gpio(soft_enable_button_gpio))
        {
            ESP_LOGW(TAG, "GPIO %d is not RTC capable, DEEP SLEEP WILL NOT WORK", soft_enable_button_gpio);
            return ESP_ERR_NOT_SUPPORTED;
        }

        rtc_gpio_deinit(soft_enable_button_gpio);

        if (soft_enable_button_active_level == 0)
        {
            rtc_gpio_pulldown_en(soft_enable_button_gpio);
            rtc_gpio_pullup_dis(soft_enable_button_gpio);
        }
        else
        {
            rtc_gpio_pullup_en(soft_enable_button_gpio);
            rtc_gpio_pulldown_dis(soft_enable_button_gpio);
        }

        esp_err_t err = esp_sleep_enable_ext0_wakeup(soft_enable_button_gpio, soft_enable_button_active_level);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to enable ext0 wakeup: %s. Restarting.", esp_err_to_name(err));
            esp_restart();
        }

        ESP_LOGI(TAG, "Wakeup source configured.");

        return ESP_OK;
    }

    void AdaApplication::shutdown()
    {
        ESP_LOGI(TAG, "Application is in shutdown.");

        esp_err_t ret = this->deinit_components();
        ESP_ERROR_CHECK(ret);

        ret = this->deinit_event_loop();
        ESP_ERROR_CHECK(ret);

        ret = this->configure_wakeup_source();
        ESP_ERROR_CHECK(ret);

        ret = this->set_status_led_state(false);
        ESP_ERROR_CHECK(ret);

        // Small delay to allow log messages to be flushed
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "Application shutdown complete");

        esp_deep_sleep_start();
    }
}
