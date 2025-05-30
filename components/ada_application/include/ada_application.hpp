#ifndef ADA_APPLICATION
#define ADA_APPLICATION

#include "esp_err.h"
#include "esp_event.h"
#include "string"
#include "esp_spiffs.h"

#include "ada_oem_data.h"

#include "ada_settings_manager.hpp"
#include "ada_speaker_driver.hpp"
#include "ada_led_strip_driver.hpp"
#include "ada_bluetooth_manager.hpp"
#include "ada_wifi_manager.hpp"
#include "ada_cloud_services.hpp"
#include "ada_microphone_driver.hpp"
#include "ada_wake_word_detection_engine.hpp"

namespace ada_assistant
{
    class AdaApplication
    {
    public:
        AdaApplication();

        esp_err_t init();

        void run_shutdown_listener();

    private:
        std::string current_firmware_version_;

        ada_oem_data_t oem_data;

        esp_event_loop_handle_t app_event_loop_handle_;

        settings_manager::AdaSettingsManager settings_manager_;
        speaker_driver::AdaSpeakerDriver speaker_driver_;
        led_strip_driver::AdaLedStripDriver led_strip_driver_;
        bluetooth_manager::AdaBluetoothManager bluetooth_manager_;
        wifi_manager::AdaWiFiManager wifi_manager_;
        cloud_services::AdaCloudServices cloud_services_;
        microphone_driver::AdaMicrophoneDriver microphone_;
        wake_word_detection_engine::AdaWakeWordDetectionEngine wake_word_engine_;

        std::string user_id_;

        gpio_num_t soft_enable_button_gpio;
        int soft_enable_button_active_level;
        int soft_enable_button_debounce_time_ms;
        int soft_enable_button_polling_rate_ms;

        gpio_num_t status_led_gpio;

        bool is_shutdown_requested;

        uint16_t calculate_oem_data_crc16();
        bool verify_oem_data_integrity();
        esp_err_t load_oem_data();

        esp_err_t mountSPIFFSPartition(char *path, char *label, size_t max_files);

        esp_err_t request_shutdown();

        void app_event_handler(esp_event_base_t event_base, int32_t event_id, void *event_data);

        static void app_event_handler_bridge(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data);

        esp_err_t init_soft_enable_button();
        bool is_soft_enable_button_on();

        esp_err_t init_status_led();
        esp_err_t set_status_led_state(bool on);

        esp_err_t init_nvs();
        esp_err_t init_event_loop();
        esp_err_t setup_initial_state();

        esp_err_t deinit_components();
        esp_err_t deinit_event_loop();

        esp_err_t configure_wakeup_source();

        void shutdown();
    };
}

#endif /* ADA_APPLICATION */
