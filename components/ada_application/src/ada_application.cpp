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
                                       speaker_driver_(),
                                       led_strip_driver_(),
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

    esp_err_t AdaApplication::mountSPIFFSPartition(char *path, char *label, size_t max_files)
    {
        ESP_LOGI(TAG, "Mounting SPIFFS %s to %s", path, label);

        esp_vfs_spiffs_conf_t conf = {
            .base_path = path,
            .partition_label = label,
            .max_files = max_files,
            .format_if_mount_failed = true};

        esp_err_t ret = esp_vfs_spiffs_register(&conf);

        if (ret != ESP_OK)
        {
            if (ret == ESP_FAIL)
            {
                ESP_LOGE(TAG, "Failed to mount or format filesystem");
            }
            else if (ret == ESP_ERR_NOT_FOUND)
            {
                ESP_LOGE(TAG, "Failed to find SPIFFS partition");
            }
            else
            {
                ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
            }
            return ret;
        }

        size_t total = 0, used = 0;
        ret = esp_spiffs_info(conf.partition_label, &total, &used);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "Mount %s to %s success", path, label);
            ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
        }

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

    esp_err_t AdaApplication::setup_initial_state()
    {
        ESP_LOGI(TAG, "Setting up initial state...");

        ESP_LOGI(TAG, "Checking app settings");
        esp_err_t ret = settings_manager_.init(app_event_loop_handle_);
        ret = settings_manager_.loadSettingsFromNvs();

        ret = mountSPIFFSPartition("/audio", "audio", 8);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
            return ESP_FAIL;
        }

        speaker_driver_.init(app_event_loop_handle_, settings_manager_.getSpeakerVolume());

        // ! LED strip brightness control here
        // led_strip_driver_.init(app_event_loop_handle_, settings_manager_.getLedStripBrightness());
        led_strip_driver_.init(app_event_loop_handle_, 1);

        if (settings_manager_.isPaired())
        {
            settings_manager::PairingData pairing_data = settings_manager_.getPairingData();
            ESP_LOGI(TAG, "Device is paired. User ID: %s, Pairing Token: %s", pairing_data.userId, pairing_data.pairingToken);
            ESP_LOGI(TAG, "Preparing for operation.");

            wifi_manager_.init(app_event_loop_handle_);

            wifi_manager_.connect_to_any_wifi(settings_manager_.getConfiguredWiFiNetworks());

            set_status_led_state(true);

            return ESP_OK;
        }

        ESP_LOGI(TAG, "Device is not paired.");
        ESP_LOGI(TAG, "Preparing for initial setup.");

        bluetooth_manager_.init(app_event_loop_handle_);
        bluetooth_manager_.start_setup_mode();

        set_status_led_state(true);
        speaker_driver_.play_mp3_file("/audio/initial_setup.mp3");
        led_strip_driver_.set_all_leds_to_color(0, 0, 255); // Blue

        return ESP_OK;
    }

    esp_err_t AdaApplication::request_shutdown()
    {
        ESP_LOGI(TAG, "Requesting shutdown...");

        esp_event_post_to(app_event_loop_handle_, ADA_APP_EVENT_BASE, APP_EVENT_DEVICE_SHUTDOWN_REQUESTED, NULL, 0, portMAX_DELAY);
        is_shutdown_requested = true;

        return ESP_OK;
    }

    esp_err_t AdaApplication::init()
    {
        ESP_LOGI(TAG, "Initializing Ada Smart Assistant application version %s", current_firmware_version_.c_str());
        ESP_LOGI(TAG, "Stack usage: %d bytes", uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

        esp_err_t ret = init_status_led();
        ESP_ERROR_CHECK(ret);

        ret = init_soft_enable_button();
        ESP_ERROR_CHECK(ret);

        if (!is_soft_enable_button_on())
        {
            ESP_LOGI(TAG, "Soft enable button is off. Shutting down.");
            is_shutdown_requested = true;

            set_status_led_state(false);

            return ESP_OK;
        }

        ESP_LOGI(TAG, "Soft enable button is on. Device is in startup.");

        ret = load_oem_data();

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to load OEM data. Shutting down.");
            is_shutdown_requested = true;

            set_status_led_state(false);

            return ret;
        }

        ret = init_nvs();
        ESP_ERROR_CHECK(ret);

        ret = init_event_loop();
        ESP_ERROR_CHECK(ret);

        ret = setup_initial_state();
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

            led_strip_driver_.set_all_leds_to_color(0, 255, 0); // Green

            break;
        }

        case APP_EVENT_BLE_DEV_CONNECTED:
        {
            ESP_LOGI(TAG, "BLE device connected");

            led_strip_driver_.start_flashing_effect({.r = 66,
                                                     .g = 135,
                                                     .b = 245,
                                                     .duration_ms = 500});
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

            ESP_LOGI(TAG, "Free internal heap: %d", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            ESP_LOGI(TAG, "Free SPIRAM heap: %d", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI(TAG, "Total free heap: %lu", esp_get_free_heap_size());

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

            cloud_services::ada_cloud_services_config_t cloud_config = {};
            cloud_config.firmware_version = current_firmware_version_.c_str();
            cloud_config.oem_data = oem_data;

            if (!settings_manager_.isPaired())
            {
                esp_err_t ret = cloud_services_.init(app_event_loop_handle_, cloud_config);
                ESP_ERROR_CHECK(ret);

                ret = cloud_services_.pair_device(user_id_);
                ESP_ERROR_CHECK(ret);

                break;
            }

            cloud_config.pairing_token = settings_manager_.getPairingData().pairingToken;
            esp_err_t ret = cloud_services_.init(app_event_loop_handle_, cloud_config);
            ESP_ERROR_CHECK(ret);

            microphone_.init(app_event_loop_handle_);

            wake_word_engine_.init(app_event_loop_handle_);
            wake_word_engine_.start();

            set_status_led_state(true);
            speaker_driver_.play_mp3_file("/audio/welcome_back.mp3");
            led_strip_driver_.set_all_leds_to_color(0, 255, 0); // Green

            break;
        }
        case APP_EVENT_WIFI_DISCONNECTED:
        {
            ESP_LOGI(TAG, "Wi-Fi disconnected");
            speaker_driver_.play_mp3_file("/audio/lost_wifi_connection.mp3");
            led_strip_driver_.set_all_leds_to_color(255, 0, 0); // Red
            led_strip_driver_.start_flashing_effect({.r = 255,
                                                     .g = 0,
                                                     .b = 0,
                                                     .duration_ms = 250});
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

            speaker_driver_.play_mp3_file("/audio/setup_complete.mp3");
            led_strip_driver_.set_all_leds_to_color(255, 165, 0); // Amber

            ret = microphone_.init(app_event_loop_handle_);
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

            speaker_driver_.play_mp3_file_blocking("/audio/listening_start.mp3");

            led_strip_driver_.start_flashing_effect({.r = 191,
                                                     .g = 245,
                                                     .b = 66,
                                                     .duration_ms = 500});

            microphone_.start_recording();
            break;
        }
        case APP_EVENT_WAKE_WORD_DETECTION_STOPPED:
        {
            ESP_LOGI(TAG, "Wake word detection stopped");
            break;
        }
        case APP_EVENT_COMMAND_RECORDING_STARTED:
        {
            ESP_LOGI(TAG, "Command recording started");

            break;
        }
        case APP_EVENT_COMMAND_RECORDING_FINISHED:
        {
            ESP_LOGI(TAG, "APP_EVENT_COMMAND_RECORDING_FINISHED event received.");

            led_strip_driver_.stop_current_effect();
            // set to flashing amber
            led_strip_driver_.start_flashing_effect({.r = 255,
                                                     .g = 165,
                                                     .b = 0,
                                                     .duration_ms = 500});

            speaker_driver_.play_mp3_file_blocking("/audio/listening_end.mp3");

            if (!event_data)
            {
                ESP_LOGE(TAG, "Event data is null for RECORDING_FINISHED. This should not happen.");
                return;
            }

            microphone_driver::event_audio_data_t *finished_payload =
                static_cast<microphone_driver::event_audio_data_t *>(event_data);

            ESP_LOGI(TAG, "Recording finished details:");
            ESP_LOGI(TAG, "  Buffer Ptr: %p", (void *)finished_payload->buffer_ptr);
            ESP_LOGI(TAG, "  Bytes Recorded: %zu", finished_payload->bytes_recorded);
            ESP_LOGI(TAG, "  Buffer Capacity: %zu bytes", finished_payload->buffer_capacity);
            ESP_LOGI(TAG, "  Sample Rate: %d Hz", finished_payload->sample_rate);
            ESP_LOGI(TAG, "  Bits Per Sample: %d", finished_payload->bits_per_sample);

            if (finished_payload->buffer_ptr != NULL)
            {
                if (finished_payload->bytes_recorded > 0)
                {
                    esp_err_t send_status = cloud_services_.sendAudio(
                        finished_payload->buffer_ptr,
                        finished_payload->bytes_recorded,
                        finished_payload->sample_rate,
                        finished_payload->bits_per_sample);

                    if (send_status == ESP_OK)
                    {
                        ESP_LOGI(TAG, "Full recording successfully sent to cloud.");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to send full recording to cloud: %s", esp_err_to_name(send_status));
                        wake_word_engine_.start();
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "Recording finished but no bytes were recorded into the buffer.");
                }

                ESP_LOGI(TAG, "Freeing recording buffer from PSRAM at %p.", (void *)finished_payload->buffer_ptr);
                heap_caps_free(finished_payload->buffer_ptr);
                finished_payload->buffer_ptr = NULL; // Good practice, though the payload struct itself is transient.
            }
            else
            {
                ESP_LOGW(TAG, "RECORDING_FINISHED event received with a NULL buffer pointer. This might indicate an earlier allocation failure or an issue.");
            }
            break;
        }
        case APP_EVENT_COMMAND_RESPONSE_RECEIVED:
        {
            ESP_LOGI(TAG, "Command processing finished event received");

            if (!event_data)
            {
                ESP_LOGE(TAG, "Event data is null for COMMAND_PROCESSING_FINISHED. This should not happen.");
                return;
            }

            cloud_services::event_command_processing_finished_data_t *finished_payload =
                static_cast<cloud_services::event_command_processing_finished_data_t *>(event_data);

            ESP_LOGI(TAG, "Command processing finished details:");
            ESP_LOGI(TAG, "  Is Command: %s", finished_payload->is_playback_start_request ? "true" : "false");
            ESP_LOGI(TAG, "  Response URL: %s", finished_payload->response_url);
            ESP_LOGI(TAG, "  Playback Audio URL: %s", finished_payload->playback_audio_url);

            led_strip_driver_.stop_current_effect();
            led_strip_driver_.set_all_leds_to_color(0, 255, 0); // Green

            speaker_driver_.play_http_stream_blocking(finished_payload->response_url);

            wake_word_engine_.start();

            if (finished_payload->is_playback_start_request)
            {
                ESP_LOGI(TAG, "Playback start request received. Starting playback.");

                speaker_driver_.play_http_stream(finished_payload->playback_audio_url);
            }
            else
            {
                ESP_LOGI(TAG, "No playback start request.");
            }

            ESP_LOGI(TAG, "Finished processing command response.");

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

        while (!is_shutdown_requested)
        {
            if (!is_soft_enable_button_on())
            {
                vTaskDelay(pdMS_TO_TICKS(soft_enable_button_debounce_time_ms));

                if (!is_soft_enable_button_on())
                {
                    ESP_LOGI(TAG, "Soft enable button is off. Shutting down.");
                    is_shutdown_requested = true;

                    break;
                }

                ESP_LOGI(TAG, "Soft enable button was off, but it is on after %d ms debounce. Ignoring.", soft_enable_button_debounce_time_ms);
            }

            vTaskDelay(pdMS_TO_TICKS(soft_enable_button_polling_rate_ms));
        }

        shutdown();
    }

    esp_err_t AdaApplication::deinit_components()
    {
        ESP_LOGI(TAG, "Deinitializing components...");

        if (wake_word_engine_.wakeWord_.is_running())
        {
            wake_word_engine_.stop();
        }

        if (microphone_.is_running())
        {
            microphone_.stop();
        }

        if (led_strip_driver_.is_effect_running())
        {
            led_strip_driver_.stop_current_effect();
        }

        led_strip_driver_.clear_led_strip();

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

        esp_err_t ret = deinit_components();
        ESP_ERROR_CHECK(ret);

        ret = deinit_event_loop();
        ESP_ERROR_CHECK(ret);

        ret = configure_wakeup_source();
        ESP_ERROR_CHECK(ret);

        ret = set_status_led_state(false);
        ESP_ERROR_CHECK(ret);

        // Small delay to allow log messages to be flushed
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "Application shutdown complete");

        esp_deep_sleep_start();
    }
}
