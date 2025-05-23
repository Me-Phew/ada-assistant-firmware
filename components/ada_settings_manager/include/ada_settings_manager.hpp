#ifndef ADA_SETTINGS_MANAGER
#define ADA_SETTINGS_MANAGER

#include "esp_err.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <array>
#include <vector>

#include "ada_global_events.hpp"

namespace ada_assistant
{
    namespace settings_manager
    {
        constexpr size_t APP_MAX_WIFI_NETWORKS = 5;

        constexpr size_t APP_MAX_SSID_LEN = 32;
        constexpr size_t APP_MAX_SSID_BUFFER_LEN = APP_MAX_SSID_LEN + 1;

        constexpr size_t APP_MAX_WIFI_PASSWORD_LEN = 64;
        constexpr size_t APP_MAX_WIFI_PASSWORD_BUFFER_LEN = APP_MAX_WIFI_PASSWORD_LEN + 1;

        struct WifiNetwork
        {
            char ssid[APP_MAX_SSID_BUFFER_LEN] = {0};
            char password[APP_MAX_WIFI_PASSWORD_BUFFER_LEN] = {0};

            bool isConfigured() const { return ssid[0] != '\0' && password[0] != '\0'; }
        };

        enum class PowerOnLedIndicationMode : uint8_t
        {
            CONSTANT = 0,
            BLINKING = 1
        };

        enum class PowerOnLedWorkingState : uint8_t
        {
            OFF = 0,
            ON = 1,
        };

        struct PowerOnLedSettings
        {
            PowerOnLedIndicationMode mode = PowerOnLedIndicationMode::CONSTANT;
            PowerOnLedWorkingState state = PowerOnLedWorkingState::ON;
        };

        constexpr size_t APP_MAX_USER_ID_LEN = 36;
        constexpr size_t APP_MAX_USER_ID_BUFFER_LEN = APP_MAX_USER_ID_LEN + 1;

        constexpr size_t MAX_PAIRING_TOKEN_LEN = 32;
        constexpr size_t MAX_PAIRING_TOKEN_BUFFER_LEN = MAX_PAIRING_TOKEN_LEN + 1;

        struct PairingData
        {
            char userId[APP_MAX_USER_ID_BUFFER_LEN] = {0};
            char pairingToken[MAX_PAIRING_TOKEN_BUFFER_LEN] = {0};

            bool isSet() const { return userId[0] != '\0' && pairingToken[0] != '\0'; }
        };

        enum class WakeWordSensitivity : uint8_t
        {
            LOW = 0,
            MEDIUM = 1,
            HIGH = 2
        };

        class AdaSettingsManager
        {
        public:
            AdaSettingsManager();

            ~AdaSettingsManager();

            esp_err_t init(esp_event_loop_handle_t app_event_loop_handle);

            esp_err_t loadSettingsFromNvs();

            esp_err_t loadDefaultSettings();

            esp_err_t resetSettings();

            // --- WiFi Settings ---
            // Gets a copy of all WiFi credentials
            const std::array<WifiNetwork, APP_MAX_WIFI_NETWORKS> &getWiFiCredentials() const;
            // Sets a specific WiFi credential. Returns ESP_OK on success, ESP_ERR_INVALID_ARG if index is out of bounds.
            esp_err_t setWiFiCredential(uint8_t index, const char *ssid, const char *password);
            // Clears a specific WiFi credential
            esp_err_t clearWiFiCredential(uint8_t index);
            // Gets the number of configured WiFi networks
            uint8_t getConfiguredWiFiNetworkCount() const;

            std::vector<WifiNetwork> getConfiguredWiFiNetworks() const;

            // Pairing data
            const PairingData &getPairingData() const;
            esp_err_t setPairingData(const char *userId, const char *pairingToken);
            bool isPaired() const; // Convenience: true if user_id and token are not empty
            esp_err_t clearPairingData();

            // --- Speaker Settings ---
            uint8_t getSpeakerVolume() const; // e.g., 0-100
            esp_err_t setSpeakerVolume(uint8_t volume);

            // --- LED Strip Settings ---
            uint8_t getLedStripBrightness() const; // e.g., 0-255
            esp_err_t setLedStripBrightness(uint8_t brightness);

            // --- Wake Word Detection Settings ---
            WakeWordSensitivity getWakeWordSensitivity() const;
            esp_err_t setWakeWordSensitivity(WakeWordSensitivity sensitivity);

            // --- Power On LED Settings ---
            const PowerOnLedSettings &getPowerOnLedSettings() const;
            esp_err_t setPowerOnLedSettings(const PowerOnLedSettings &settings);

        private:
            esp_event_loop_handle_t app_event_loop_handle_;

            // NVS Namespace and Keys (static const char* for keys is good practice)
            static const char *NVS_NAMESPACE;
            static const char *NVS_KEY_WIFI_NETWORKS;
            static const char *NVS_KEY_PAIRING_DATA;
            static const char *NVS_KEY_SPEAKER_VOL;
            static const char *NVS_KEY_LED_BRIGHTNESS;
            static const char *NVS_KEY_WAKE_WORD_SENSITIVITY;
            static const char *NVS_KEY_POWER_LED_SETTINGS;

            // Default values
            // For structs, you might define them fully in the .cpp file if complex,
            // or use C++11 initializers here if simple.
            static const uint8_t DEFAULT_SPEAKER_VOLUME = 100;
            static const uint8_t DEFAULT_LED_STRIP_BRIGHTNESS = 128;
            static const WakeWordSensitivity DEFAULT_WAKE_WORD_SENSITIVITY = WakeWordSensitivity::MEDIUM;
            // C++11 in-class static const member initialization for the struct
            static const PowerOnLedSettings DEFAULT_POWER_ON_LED_SETTINGS;
            // Note: For wifi_networks_ and pairing_data_, defaults are typically "empty" or "unconfigured",
            // which the char arrays initialized to {0} and `isPlaceholder=true` already handle.

            // NVS and Eventing
            nvs_handle_t nvs_handle_;
            bool is_nvs_open_ = false;

            esp_err_t open_nvs();
            void close_nvs();
            esp_err_t post_settings_updated_event(int32_t event_id = APP_EVENT_SETTINGS_CHANGED, void *event_data = nullptr, size_t event_data_size = 0);

            esp_err_t saveAllSettingsToNvs();

            esp_err_t saveWiFiSettingsToNvs();
            esp_err_t savePairingDataToNvs();
            esp_err_t saveSpeakerVolumeToNvs();
            esp_err_t saveLedStripBrightnessToNvs();
            esp_err_t saveWakeWordSensitivityToNvs();
            esp_err_t savePowerOnLedSettingsToNvs();

            // Member variables to hold current settings
            std::array<WifiNetwork, APP_MAX_WIFI_NETWORKS> wifi_networks_;
            PairingData pairing_data_;
            uint8_t speaker_volume_;
            uint8_t led_strip_brightness_;
            WakeWordSensitivity wake_word_sensitivity_;
            PowerOnLedSettings power_on_led_settings_;
        };
    } // namespace settings_manager
} // namespace ada_assistant

#endif /* ADA_SETTINGS_MANAGER */
