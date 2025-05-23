#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_partition.h"
#include "string.h"

#include "ada_firmware_version.hpp"
#include "ada_oem_data.h"
#include "ada_global_events.hpp"
#include "ada_application.hpp"
#include "esp_rom_crc.h"

ESP_EVENT_DEFINE_BASE(ADA_APP_EVENT_BASE);

namespace ada_assistant
{
    static const char *TAG = "ADA_APPLICATION";

    AdaApplication::AdaApplication() : current_firmware_version_(FIRMWARE_VERSION_STRING),
                                       oem_data(),
                                       app_event_loop_handle_(nullptr),
                                       settings_manager_(),
                                       bluetooth_manager_(),
                                       wifi_manager_(),
                                       cloud_services_(),
                                       microphone_(), wake_word_engine_({
                                                          .microphone = &microphone_,
                                                      }),
                                       soft_enable_button_gpio(static_cast<gpio_num_t>(CONFIG_ADA_APP_EN_BTN_GPIO)), soft_enable_button_active_level(CONFIG_ADA_APP_EN_BTN_ACTIVE_LEVEL), soft_enable_button_debounce_time_ms(CONFIG_ADA_APP_EN_BTN_DEBOUNCE_TIME_MS), soft_enable_button_polling_rate_ms(CONFIG_ADA_APP_EN_BTN_POLLING_RATE_MS), status_led_gpio(static_cast<gpio_num_t>(CONFIG_ADA_APP_EN_LED_GPIO)), is_shutdown_requested(false)

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

    uint16_t AdaApplication::calculate_oem_data_crc16()
    {
        ada_oem_data_t temp_data = oem_data;
        temp_data.crc16 = 0; // Zero out the CRC field for calculation

        // Calculate CRC over the entire temporary structure
        const uint8_t *data_bytes = reinterpret_cast<const uint8_t *>(&temp_data);
        uint16_t calculated_crc = esp_rom_crc16_le(0, data_bytes, sizeof(ada_oem_data_t));

        return calculated_crc;
    }

    bool AdaApplication::verify_oem_data_integrity()
    {
        ESP_LOGI(TAG, "Verifying OEM data integrity...");

        if (oem_data.magic_number != OEM_DATA_MAGIC_NUMBER)
        {
            ESP_LOGE(TAG, "Magic number mismatch. Expected: 0x%08lX, Got: 0x%08lX",
                     (unsigned long)OEM_DATA_MAGIC_NUMBER, (unsigned long)oem_data.magic_number);
            return false;
        }

        if (oem_data.struct_version != OEM_STRUCT_VERSION)
        {
            ESP_LOGW(TAG, "Manufacturing data version mismatch or unexpected: %u", oem_data.struct_version);
        }

        uint16_t expected_crc = oem_data.crc16;
        uint16_t calculated_crc = calculate_oem_data_crc16();

        if (expected_crc != calculated_crc)
        {
            ESP_LOGE(TAG, "CRC mismatch. Expected (from struct): 0x%04X, Calculated: 0x%04X",
                     expected_crc, calculated_crc);
            return false;
        }

        ESP_LOGI(TAG, "OEM Data verified successfully (Magic: OK, CRC: OK).");
        return true;
    }

    esp_err_t AdaApplication::load_oem_data()
    {
        ESP_LOGI(TAG, "Loading OEM data...");

        const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                    (esp_partition_subtype_t)0x40,
                                                                    "oem_data");

        if (!partition)
        {
            ESP_LOGE(TAG, "Manufacturing data partition 'oem_data' not found!");
            return ESP_ERR_NOT_FOUND;
        }

        ESP_LOGI(TAG, "Found oem_data partition: size %lu, offset 0x%lx", partition->size, partition->address);

        if (partition->size < sizeof(ada_oem_data_t))
        {
            ESP_LOGE(TAG, "OEM partition is too small for ada_oem_data_t struct!");
            return ESP_FAIL;
        }

        esp_err_t err = esp_partition_read(partition, 0, &oem_data, sizeof(ada_oem_data_t));
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read oem_data partition: %s", esp_err_to_name(err));
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Raw data read. Magic: 0x%08lX, Version: %u, Stored CRC: 0x%04X",
                 oem_data.magic_number, oem_data.struct_version, oem_data.crc16);

        if (!verify_oem_data_integrity())
        {
            ESP_LOGE(TAG, "Failed to verify OEM data integrity.");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "OEM data loaded and verified successfully.");
        ESP_LOGI(TAG, "Board Revision: %s", oem_data.board_revision);
        ESP_LOGI(TAG, "Factory Firmware Version: %s", oem_data.factory_firmware_version);
        ESP_LOGI(TAG, "Serial Number: %s", oem_data.serial_number);

        return ESP_OK;
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

    // esp_err_t AdaApplication::init_components()
    // {
    //     ESP_LOGI(TAG, "Initializing components...");

    //     this->settings_manager_.init(app_event_loop_handle_);
    //     this->settings_manager_.loadSettingsFromNvs();

    //     this->microphone_.init();

    //     this->wake_word_engine_.init(app_event_loop_handle_);
    //     this->wake_word_engine_.start();

    //     this->bluetooth_manager_.init(app_event_loop_handle_);

    //     ESP_LOGI(TAG, "Component initialization complete.");

    //     return ESP_OK;
    // }

    esp_err_t AdaApplication::setup_initial_state()
    {
        ESP_LOGI(TAG, "Setting up initial state...");

        ESP_LOGI(TAG, "Checking app settings");
        this->settings_manager_.init(app_event_loop_handle_);
        this->settings_manager_.loadSettingsFromNvs();

        if (this->settings_manager_.isPaired())
        {
            settings_manager::PairingData pairing_data = this->settings_manager_.getPairingData();
            ESP_LOGI(TAG, "Device is paired. User ID: %s, Pairing Token: %s", pairing_data.userId, pairing_data.pairingToken);
            ESP_LOGI(TAG, "Preparing for operation.");

            return ESP_OK;
        }

        ESP_LOGI(TAG, "Device is not paired.");
        ESP_LOGI(TAG, "Preparing for initial setup.");

        this->bluetooth_manager_.init(app_event_loop_handle_);
        this->bluetooth_manager_.start_setup_mode();

        return ESP_OK;
    }

    esp_err_t AdaApplication::request_shutdown()
    {
        ESP_LOGI(TAG, "Requesting shutdown...");

        esp_event_post_to(app_event_loop_handle_, ADA_APP_EVENT_BASE, APP_EVENT_DEVICE_SHUTDOWN_REQUESTED, NULL, 0, portMAX_DELAY);
        this->is_shutdown_requested = true;

        return ESP_OK;
    }

    esp_err_t AdaApplication::init()
    {
        ESP_LOGI(TAG, "Initializing Ada Smart Assistant application version %s", current_firmware_version_.c_str());
        ESP_LOGI(TAG, "Stack usage: %d bytes", uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

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

        ret = this->load_oem_data();
        ESP_ERROR_CHECK(ret);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to load OEM data. Shutting down.");
            this->is_shutdown_requested = true;

            set_status_led_state(false);

            return ret;
        }

        ret = this->init_nvs();
        ESP_ERROR_CHECK(ret);

        ret = this->init_event_loop();
        ESP_ERROR_CHECK(ret);

        ret = this->setup_initial_state();
        ESP_ERROR_CHECK(ret);

        ESP_LOGI(TAG, "Application initialization finished. Handing off control to the event loop.");
        ESP_LOGI(TAG, "Stack usage: %d bytes", uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(100));
        esp_event_post_to(app_event_loop_handle_, ADA_APP_EVENT_BASE, APP_EVENT_DEVICE_READY, NULL, 0, portMAX_DELAY);

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
        case APP_EVENT_DEVICE_READY:
        {
            ESP_LOGI(TAG, "Main loop control handoff successful");
            set_status_led_state(true);
            break;
        }

        case APP_EVENT_BLE_SETUP_DATA_RECEIVED:
        {
            ESP_LOGI(TAG, "BLE setup data received");

            if (!event_data)
            {
                ESP_LOGE(TAG, "Event data is null");
                return;
            }

            bluetooth_manager::setup_event_data_t *setup_data = static_cast<bluetooth_manager::setup_event_data_t *>(event_data);
            ESP_LOGI(TAG, "Received SSID: %s", setup_data->ssid);
            ESP_LOGI(TAG, "Received Password, length= %d", strlen(setup_data->password));
            ESP_LOGI(TAG, "Received User ID: %s", setup_data->user_id);

            user_id_ = setup_data->user_id;

            esp_err_t ret = wifi_manager_.init(app_event_loop_handle_);
            ESP_ERROR_CHECK(ret);

            ret = wifi_manager_.connect_to_wifi(setup_data->ssid, setup_data->password);
            ESP_ERROR_CHECK(ret);

            // TODO Send progress to BT manager

            break;
        }

        case APP_EVENT_WIFI_CONNECTION_FAILED:
        {
            ESP_LOGI(TAG, "Wi-Fi connection failed");
            // TODO Ask the BT manager for a new SSID and password pair

            break;
        }
        case APP_EVENT_WIFI_CONNECTED:
        {
            ESP_LOGI(TAG, "Wi-Fi connected");
            // TODO Send progress to BT manager

            settings_manager_.setWiFiCredential(1, wifi_manager_.getCurrentNetworkSSID().c_str(), wifi_manager_.getCurrentNetworkPassword().c_str());

            cloud_services::ada_cloud_services_config_t cloud_config;
            cloud_config.firmware_version = current_firmware_version_.c_str();
            cloud_config.oem_data = oem_data;

            esp_err_t ret = cloud_services_.init(app_event_loop_handle_, cloud_config);
            ESP_ERROR_CHECK(ret);

            ret = cloud_services_.pair_device(user_id_);
            ESP_ERROR_CHECK(ret);
            break;
        }
        case APP_EVENT_PAIRING_COMPLETED:
        {
            esp_err_t ret = settings_manager_.setPairingData(user_id_.c_str(), cloud_services_.getPairingToken().c_str());
            ESP_ERROR_CHECK(ret);

            ret = cloud_services_.connect_to_gateway();
            ESP_ERROR_CHECK(ret);

            ret = bluetooth_manager_.finalize_setup_and_disable_ble();
            ESP_ERROR_CHECK(ret);

            // TODO Sound

            ret = microphone_.init();
            ESP_ERROR_CHECK(ret);

            ret = wake_word_engine_.init(app_event_loop_handle_);
            ESP_ERROR_CHECK(ret);

            ret = wake_word_engine_.start();
            ESP_ERROR_CHECK(ret);

            break;
        }
        case APP_EVENT_WAKE_WORD_DETECTED:
        {
            ESP_LOGI(TAG, "Wake word detected");
            break;
        }
        case APP_EVENT_WAKE_WORD_DETECTION_STOPPED:
        {
            ESP_LOGI(TAG, "Wake word detection stopped");
            break;
        }
        default:
        {
            ESP_LOGW(TAG, "Ignoring unhandled event: %ld", event_id);
            break;
        }
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
