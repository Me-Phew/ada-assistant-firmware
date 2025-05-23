#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <cstring>

#include "ada_settings_manager.hpp"

namespace ada_assistant
{
    namespace settings_manager
    {
        static const char *TAG = "ADA_SETTINGS_MANAGER";

        // --- NVS Key Definitions ---
        const char *AdaSettingsManager::NVS_NAMESPACE = "ada_settings";
        const char *AdaSettingsManager::NVS_KEY_INITIALIZED_FLAG = "initialized";
        const char *AdaSettingsManager::NVS_KEY_WIFI_NETWORKS = "wifi_nets";
        const char *AdaSettingsManager::NVS_KEY_PAIRING_DATA = "pairing_data";
        const char *AdaSettingsManager::NVS_KEY_SPEAKER_VOL = "spk_vol";
        const char *AdaSettingsManager::NVS_KEY_LED_BRIGHTNESS = "led_bright";
        const char *AdaSettingsManager::NVS_KEY_WAKE_WORD_SENSITIVITY = "ww_sens";
        const char *AdaSettingsManager::NVS_KEY_POWER_LED_SETTINGS = "pwr_led_cfg";

        // --- Default Value Definitions ---
        const PowerOnLedSettings AdaSettingsManager::DEFAULT_POWER_ON_LED_SETTINGS = {
            PowerOnLedIndicationMode::CONSTANT,
            PowerOnLedWorkingState::ON};
        // Other defaults are defined in the header or initialized directly.

        AdaSettingsManager::AdaSettingsManager()
            : app_event_loop_handle_(nullptr),
              nvs_handle_(0), // Initialize nvs_handle_
              is_nvs_open_(false),
              speaker_volume_(DEFAULT_SPEAKER_VOLUME),
              led_strip_brightness_(DEFAULT_LED_STRIP_BRIGHTNESS),
              wake_word_sensitivity_(DEFAULT_WAKE_WORD_SENSITIVITY),
              power_on_led_settings_(DEFAULT_POWER_ON_LED_SETTINGS)
        {
            // Initialize structs with their default empty/placeholder states
            // wifi_networks_ are default initialized by WifiNetwork constructor (isPlaceholder = true)
            // pairing_data_ is default initialized by PairingData constructor (char arrays are zeroed)
            ESP_LOGI(TAG, "AdaSettingsManager instance created.");
        }

        AdaSettingsManager::~AdaSettingsManager()
        {
            close_nvs();
            ESP_LOGI(TAG, "AdaSettingsManager instance destroyed.");
        }

        esp_err_t AdaSettingsManager::open_nvs()
        {
            if (is_nvs_open_)
            {
                return ESP_OK;
            }
            esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle_);
            if (err == ESP_OK)
            {
                is_nvs_open_ = true;
                ESP_LOGI(TAG, "NVS namespace '%s' opened successfully.", NVS_NAMESPACE);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
            }
            return err;
        }

        void AdaSettingsManager::close_nvs()
        {
            if (is_nvs_open_)
            {
                nvs_close(nvs_handle_);
                is_nvs_open_ = false;
                nvs_handle_ = 0; // Reset handle
                ESP_LOGI(TAG, "NVS namespace '%s' closed.", NVS_NAMESPACE);
            }
        }

        esp_err_t AdaSettingsManager::init(esp_event_loop_handle_t app_event_loop_handle)
        {
            ESP_LOGI(TAG, "Initializing AdaSettingsManager...");
            app_event_loop_handle_ = app_event_loop_handle;

            esp_err_t ret = open_nvs();
            if (ret != ESP_OK)
            {
                // Already logged in open_nvs
                return ret;
            }

            if (nvsHasStoredSettings())
            {
                ESP_LOGI(TAG, "Found stored settings in NVS. Loading them.");
                ret = loadSettingsFromNvs();

                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to load settings from NVS: %s. Loading defaults.", esp_err_to_name(ret));
                    // Fallback to defaults if loading fails
                    ret = loadDefaultSettings();
                }

                return ret;
            }

            ESP_LOGI(TAG, "No stored settings found in NVS or initialization flag missing. Loading default settings.");
            ret = loadDefaultSettings();

            return ret;
        }

        bool AdaSettingsManager::nvsHasStoredSettings()
        {
            if (!is_nvs_open_)
            {
                ESP_LOGE(TAG, "NVS not open in loadSettingsFromNvs.");
                return ESP_ERR_NVS_NOT_INITIALIZED;
            }

            uint8_t initialized_flag = 0;
            esp_err_t err = nvs_get_u8(nvs_handle_, NVS_KEY_INITIALIZED_FLAG, &initialized_flag);

            if (err == ESP_OK && initialized_flag == 1)
            {
                ESP_LOGD(TAG, "NVS_KEY_INITIALIZED_FLAG found and set to 1.");
                return true;
            }

            if (err == ESP_ERR_NVS_NOT_FOUND)
            {
                ESP_LOGI(TAG, "NVS_KEY_INITIALIZED_FLAG not found.");
                return false;
            }

            ESP_LOGE(TAG, "Error reading NVS_KEY_INITIALIZED_FLAG: %s", esp_err_to_name(err));

            return false;
        }

        esp_err_t AdaSettingsManager::loadSettingsFromNvs()
        {
            if (!is_nvs_open_)
            {
                ESP_LOGE(TAG, "NVS not open in loadSettingsFromNvs.");
                return ESP_ERR_NVS_NOT_INITIALIZED;
            }

            esp_err_t err;
            size_t required_size;

            // Load WiFi Networks
            required_size = sizeof(wifi_networks_);
            err = nvs_get_blob(nvs_handle_, NVS_KEY_WIFI_NETWORKS, wifi_networks_.data(), &required_size);
            if (err != ESP_OK || required_size != sizeof(wifi_networks_))
            {
                ESP_LOGW(TAG, "Failed to load WiFi settings or size mismatch (%s). Using defaults for WiFi.", esp_err_to_name(err));

                for (auto &net : wifi_networks_)
                {
                    memset(net.ssid, 0, sizeof(net.ssid));
                    memset(net.password, 0, sizeof(net.password));
                }
            }
            else
            {
                ESP_LOGI(TAG, "WiFi settings loaded from NVS.");
            }

            // Load Pairing Data
            required_size = sizeof(pairing_data_);
            err = nvs_get_blob(nvs_handle_, NVS_KEY_PAIRING_DATA, &pairing_data_, &required_size);
            if (err != ESP_OK || required_size != sizeof(pairing_data_))
            {
                ESP_LOGW(TAG, "Failed to load pairing data or size mismatch (%s). Using defaults for pairing.", esp_err_to_name(err));
                memset(&pairing_data_, 0, sizeof(pairing_data_)); // Clear pairing data
            }
            else
            {
                ESP_LOGI(TAG, "Pairing data loaded from NVS.");
            }

            // Load Speaker Volume
            err = nvs_get_u8(nvs_handle_, NVS_KEY_SPEAKER_VOL, &speaker_volume_);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to load speaker volume (%s). Using default: %u", esp_err_to_name(err), DEFAULT_SPEAKER_VOLUME);
                speaker_volume_ = DEFAULT_SPEAKER_VOLUME;
            }
            else
            {
                ESP_LOGI(TAG, "Speaker volume loaded: %u", speaker_volume_);
            }

            // Load LED Strip Brightness
            err = nvs_get_u8(nvs_handle_, NVS_KEY_LED_BRIGHTNESS, &led_strip_brightness_);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to load LED strip brightness (%s). Using default: %u", esp_err_to_name(err), DEFAULT_LED_STRIP_BRIGHTNESS);
                led_strip_brightness_ = DEFAULT_LED_STRIP_BRIGHTNESS;
            }
            else
            {
                ESP_LOGI(TAG, "LED strip brightness loaded: %u", led_strip_brightness_);
            }

            // Load Wake Word Sensitivity
            // NVS stores enum as underlying type (uint8_t)
            uint8_t sensitivity_val;
            err = nvs_get_u8(nvs_handle_, NVS_KEY_WAKE_WORD_SENSITIVITY, &sensitivity_val);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to load wake word sensitivity (%s). Using default.", esp_err_to_name(err));
                wake_word_sensitivity_ = DEFAULT_WAKE_WORD_SENSITIVITY;
            }
            else
            {
                // Add validation if needed (e.g., if value is outside enum range)
                wake_word_sensitivity_ = static_cast<WakeWordSensitivity>(sensitivity_val);
                ESP_LOGI(TAG, "Wake word sensitivity loaded: %u", static_cast<uint8_t>(wake_word_sensitivity_));
            }

            // Load Power On LED Settings
            required_size = sizeof(power_on_led_settings_);
            err = nvs_get_blob(nvs_handle_, NVS_KEY_POWER_LED_SETTINGS, &power_on_led_settings_, &required_size);
            if (err != ESP_OK || required_size != sizeof(power_on_led_settings_))
            {
                ESP_LOGW(TAG, "Failed to load power on LED settings or size mismatch (%s). Using defaults.", esp_err_to_name(err));
                power_on_led_settings_ = DEFAULT_POWER_ON_LED_SETTINGS;
            }
            else
            {
                ESP_LOGI(TAG, "Power on LED settings loaded. Mode: %d, State: %d",
                         static_cast<int>(power_on_led_settings_.mode),
                         static_cast<int>(power_on_led_settings_.state));
            }

            // If any load failed, it might be prudent to return an error,
            // but for robustness, we've loaded defaults for failed items.
            // The function should return ESP_OK if it managed to get into a valid state.
            ESP_LOGI(TAG, "All settings loaded from NVS (or defaults applied for missing items).");
            return ESP_OK;
        }

        esp_err_t AdaSettingsManager::saveAllSettingsToNvs()
        {
            if (!is_nvs_open_)
            {
                ESP_LOGE(TAG, "NVS not open in saveAllSettingsToNvs.");
                return ESP_ERR_NVS_NOT_INITIALIZED;
            }
            ESP_LOGI(TAG, "Saving all settings to NVS...");

            esp_err_t err;

            // Save individual groups
            err = saveWiFiSettingsToNvs();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to save WiFi settings: %s", esp_err_to_name(err));
                return err;
            }
            err = savePairingDataToNvs();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to save pairing data: %s", esp_err_to_name(err));
                return err;
            }
            err = saveSpeakerVolumeToNvs();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to save speaker volume: %s", esp_err_to_name(err));
                return err;
            }
            err = saveLedStripBrightnessToNvs();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to save LED brightness: %s", esp_err_to_name(err));
                return err;
            }
            err = saveWakeWordSensitivityToNvs();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to save wake word sensitivity: %s", esp_err_to_name(err));
                return err;
            }
            err = savePowerOnLedSettingsToNvs();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to save power LED settings: %s", esp_err_to_name(err));
                return err;
            }

            // After all settings are successfully saved, set the initialized flag
            uint8_t initialized_flag = 1;
            err = nvs_set_u8(nvs_handle_, NVS_KEY_INITIALIZED_FLAG, initialized_flag);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to set NVS_KEY_INITIALIZED_FLAG: %s", esp_err_to_name(err));
                return err;
            }

            // Commit all changes
            err = nvs_commit(nvs_handle_);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
                return err;
            }

            ESP_LOGI(TAG, "All settings successfully saved to NVS and committed.");
            post_settings_updated_event(); // Post a generic event after all settings are saved
            return ESP_OK;
        }

        esp_err_t AdaSettingsManager::loadDefaultSettings()
        {
            ESP_LOGI(TAG, "Loading default settings...");

            for (auto &network : wifi_networks_)
            {
                memset(network.ssid, 0, sizeof(network.ssid));
                memset(network.password, 0, sizeof(network.password));
            }

            memset(&pairing_data_, 0, sizeof(pairing_data_));

            speaker_volume_ = DEFAULT_SPEAKER_VOLUME;
            led_strip_brightness_ = DEFAULT_LED_STRIP_BRIGHTNESS;
            wake_word_sensitivity_ = DEFAULT_WAKE_WORD_SENSITIVITY;
            power_on_led_settings_ = DEFAULT_POWER_ON_LED_SETTINGS;

            ESP_LOGI(TAG, "Default settings loaded into memory.");
            return ESP_OK;
        }

        esp_err_t AdaSettingsManager::resetSettings()
        {
            if (!is_nvs_open_)
            {
                ESP_LOGE(TAG, "NVS not open in saveAllSettingsToNvs.");
                return ESP_ERR_NVS_NOT_INITIALIZED;
            }

            ESP_LOGW(TAG, "Resetting all settings to defaults and erasing from NVS...");

            esp_err_t err = nvs_erase_all(nvs_handle_);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to erase NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
                // Attempt to commit anyway to clear what might have been erased
                nvs_commit(nvs_handle_);
                return err; // Return error, but proceed to load defaults in memory
            }

            err = nvs_commit(nvs_handle_);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to commit NVS erase: %s", esp_err_to_name(err));
                // Even if commit fails, proceed to load defaults in memory.
                // The NVS state might be inconsistent, but app needs to run.
            }

            ESP_LOGI(TAG, "NVS erased. Loading default settings...");
            // Load defaults into memory and then save them back to NVS (which will also set the init flag)
            return loadDefaultSettings();
        }

        esp_err_t AdaSettingsManager::post_settings_updated_event(int32_t event_id, void *event_data, size_t event_data_size)
        {
            if (app_event_loop_handle_ == nullptr)
            {
                ESP_LOGW(TAG, "Application event loop handle is null. Cannot post settings updated event.");
                return ESP_ERR_INVALID_STATE;
            }

            esp_err_t err = esp_event_post_to(app_event_loop_handle_, ADA_APP_EVENT_BASE, event_id,
                                              event_data, event_data_size, pdMS_TO_TICKS(100));
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to post settings updated event: %s", esp_err_to_name(err));
            }
            else
            {
                ESP_LOGD(TAG, "Posted settings updated event ID: %ld", event_id);
            }
            return err;
        }

        esp_err_t AdaSettingsManager::saveWiFiSettingsToNvs()
        {
            if (!is_nvs_open_)
                return ESP_ERR_NVS_NOT_INITIALIZED;
            ESP_LOGD(TAG, "Saving WiFi settings to NVS...");
            esp_err_t err = nvs_set_blob(nvs_handle_, NVS_KEY_WIFI_NETWORKS, wifi_networks_.data(), sizeof(wifi_networks_));
            // Individual save helpers generally should not commit here if part of a larger save operation (like saveAllSettingsToNvs)
            // However, if setters call these directly AND expect immediate persistence, then commit (or a flag to control commit).
            // For now, assume saveAllSettingsToNvs or setters will handle commit logic.
            // If a setter calls this, it should then call nvs_commit and post_settings_updated_event.
            // Let's make setters responsible for commit and event.
            return err;
        }

        esp_err_t AdaSettingsManager::savePairingDataToNvs()
        {
            if (!is_nvs_open_)
                return ESP_ERR_NVS_NOT_INITIALIZED;
            ESP_LOGD(TAG, "Saving pairing data to NVS...");
            return nvs_set_blob(nvs_handle_, NVS_KEY_PAIRING_DATA, &pairing_data_, sizeof(pairing_data_));
        }

        esp_err_t AdaSettingsManager::saveSpeakerVolumeToNvs()
        {
            if (!is_nvs_open_)
                return ESP_ERR_NVS_NOT_INITIALIZED;
            ESP_LOGD(TAG, "Saving speaker volume (%u) to NVS...", speaker_volume_);
            return nvs_set_u8(nvs_handle_, NVS_KEY_SPEAKER_VOL, speaker_volume_);
        }

        esp_err_t AdaSettingsManager::saveLedStripBrightnessToNvs()
        {
            if (!is_nvs_open_)
                return ESP_ERR_NVS_NOT_INITIALIZED;
            ESP_LOGD(TAG, "Saving LED brightness (%u) to NVS...", led_strip_brightness_);
            return nvs_set_u8(nvs_handle_, NVS_KEY_LED_BRIGHTNESS, led_strip_brightness_);
        }

        esp_err_t AdaSettingsManager::saveWakeWordSensitivityToNvs()
        {
            if (!is_nvs_open_)
                return ESP_ERR_NVS_NOT_INITIALIZED;
            ESP_LOGD(TAG, "Saving wake word sensitivity (%u) to NVS...", static_cast<uint8_t>(wake_word_sensitivity_));
            return nvs_set_u8(nvs_handle_, NVS_KEY_WAKE_WORD_SENSITIVITY, static_cast<uint8_t>(wake_word_sensitivity_));
        }

        esp_err_t AdaSettingsManager::savePowerOnLedSettingsToNvs()
        {
            if (!is_nvs_open_)
                return ESP_ERR_NVS_NOT_INITIALIZED;
            ESP_LOGD(TAG, "Saving power LED settings to NVS...");
            return nvs_set_blob(nvs_handle_, NVS_KEY_POWER_LED_SETTINGS, &power_on_led_settings_, sizeof(power_on_led_settings_));
        }

        // --- Getter Implementations ---
        const std::array<WifiNetwork, APP_MAX_WIFI_NETWORKS> &AdaSettingsManager::getWiFiCredentials() const
        {
            return wifi_networks_;
        }

        uint8_t AdaSettingsManager::getConfiguredWiFiNetworkCount() const
        {
            uint8_t count = 0;
            for (const auto &net : wifi_networks_)
            {
                if (net.isConfigured())
                {
                    count++;
                }
            }
            return count;
        }

        const PairingData &AdaSettingsManager::getPairingData() const
        {
            return pairing_data_;
        }

        bool AdaSettingsManager::isPaired() const
        {
            return pairing_data_.isSet();
        }

        uint8_t AdaSettingsManager::getSpeakerVolume() const
        {
            return speaker_volume_;
        }

        uint8_t AdaSettingsManager::getLedStripBrightness() const
        {
            return led_strip_brightness_;
        }

        WakeWordSensitivity AdaSettingsManager::getWakeWordSensitivity() const
        {
            return wake_word_sensitivity_;
        }

        const PowerOnLedSettings &AdaSettingsManager::getPowerOnLedSettings() const
        {
            return power_on_led_settings_;
        }

        // --- Setter Implementations ---
        esp_err_t AdaSettingsManager::setWiFiCredential(uint8_t index, const char *ssid, const char *password)
        {
            if (index >= APP_MAX_WIFI_NETWORKS)
            {
                ESP_LOGE(TAG, "setWiFiCredential: Index %u out of bounds (max %zu)", index, APP_MAX_WIFI_NETWORKS - 1);
                return ESP_ERR_INVALID_ARG;
            }
            if (!ssid || !password)
            {
                ESP_LOGE(TAG, "setWiFiCredential: SSID or password cannot be null.");
                return ESP_ERR_INVALID_ARG;
            }
            if (strlen(ssid) > APP_MAX_SSID_LEN)
            {
                ESP_LOGE(TAG, "setWiFiCredential: SSID too long (max %zu)", APP_MAX_SSID_LEN);
                return ESP_ERR_INVALID_ARG;
            }
            if (strlen(password) > APP_MAX_WIFI_PASSWORD_LEN)
            {
                ESP_LOGE(TAG, "setWiFiCredential: Password too long (max %zu)", APP_MAX_WIFI_PASSWORD_LEN);
                return ESP_ERR_INVALID_ARG;
            }

            WifiNetwork &net = wifi_networks_[index];
            strncpy(net.ssid, ssid, APP_MAX_SSID_LEN);
            net.ssid[APP_MAX_SSID_LEN] = '\0'; // Ensure null termination
            strncpy(net.password, password, APP_MAX_WIFI_PASSWORD_LEN);
            net.password[APP_MAX_WIFI_PASSWORD_LEN] = '\0'; // Ensure null termination

            ESP_LOGI(TAG, "Set WiFi credential for index %u, SSID: %s", index, net.ssid);

            esp_err_t err = saveWiFiSettingsToNvs();
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs_handle_);
                if (err == ESP_OK)
                {
                    post_settings_updated_event(APP_EVENT_SETTINGS_CHANGED);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to commit NVS after setting WiFi credential: %s", esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to save WiFi settings to NVS: %s", esp_err_to_name(err));
            }
            return err;
        }

        esp_err_t AdaSettingsManager::clearWiFiCredential(uint8_t index)
        {
            if (index >= APP_MAX_WIFI_NETWORKS)
            {
                ESP_LOGE(TAG, "clearWiFiCredential: Index %u out of bounds", index);
                return ESP_ERR_INVALID_ARG;
            }

            WifiNetwork &net = wifi_networks_[index];
            memset(net.ssid, 0, sizeof(net.ssid));
            memset(net.password, 0, sizeof(net.password));

            ESP_LOGI(TAG, "Cleared WiFi credential for index %u", index);

            esp_err_t err = saveWiFiSettingsToNvs();
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs_handle_);
                if (err == ESP_OK)
                {
                    post_settings_updated_event(APP_EVENT_SETTINGS_CHANGED);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to commit NVS after clearing WiFi credential: %s", esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to save WiFi settings to NVS: %s", esp_err_to_name(err));
            }
            return err;
        }

        esp_err_t AdaSettingsManager::setPairingData(const char *userId, const char *pairingToken)
        {
            if (!userId || !pairingToken)
            {
                ESP_LOGE(TAG, "setPairingData: User ID or Token cannot be null.");
                return ESP_ERR_INVALID_ARG;
            }
            if (strlen(userId) > APP_MAX_USER_ID_LEN)
            {
                ESP_LOGE(TAG, "setPairingData: User ID too long (max %zu)", APP_MAX_USER_ID_LEN);
                return ESP_ERR_INVALID_ARG;
            }
            if (strlen(pairingToken) > MAX_PAIRING_TOKEN_LEN)
            {
                ESP_LOGE(TAG, "setPairingData: Pairing token too long (max %zu)", MAX_PAIRING_TOKEN_LEN);
                return ESP_ERR_INVALID_ARG;
            }

            strncpy(pairing_data_.userId, userId, APP_MAX_USER_ID_LEN);
            pairing_data_.userId[APP_MAX_USER_ID_LEN] = '\0';
            strncpy(pairing_data_.pairingToken, pairingToken, MAX_PAIRING_TOKEN_LEN);
            pairing_data_.pairingToken[MAX_PAIRING_TOKEN_LEN] = '\0';

            ESP_LOGI(TAG, "Set pairing data. UserID: %s", pairing_data_.userId);

            esp_err_t err = savePairingDataToNvs();
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs_handle_);
                if (err == ESP_OK)
                {
                    post_settings_updated_event(APP_EVENT_SETTINGS_CHANGED);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to commit NVS after setting pairing data: %s", esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to save pairing data to NVS: %s", esp_err_to_name(err));
            }
            return err;
        }

        esp_err_t AdaSettingsManager::clearPairingData()
        {
            memset(&pairing_data_, 0, sizeof(pairing_data_));
            ESP_LOGI(TAG, "Cleared pairing data.");

            esp_err_t err = savePairingDataToNvs();
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs_handle_);
                if (err == ESP_OK)
                {
                    post_settings_updated_event(APP_EVENT_SETTINGS_CHANGED);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to commit NVS after clearing pairing data: %s", esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to save pairing data to NVS: %s", esp_err_to_name(err));
            }
            return err;
        }

        esp_err_t AdaSettingsManager::setSpeakerVolume(uint8_t volume)
        {
            if (volume > 100)
            { // Assuming 0-100 range
                ESP_LOGW(TAG, "setSpeakerVolume: Volume %u out of range (0-100). Clamping to 100.", volume);
                volume = 100;
            }
            speaker_volume_ = volume;
            ESP_LOGI(TAG, "Set speaker volume to: %u", speaker_volume_);

            esp_err_t err = saveSpeakerVolumeToNvs();
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs_handle_);
                if (err == ESP_OK)
                {
                    post_settings_updated_event(APP_EVENT_SETTINGS_CHANGED);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to commit NVS after setting speaker volume: %s", esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to save speaker volume to NVS: %s", esp_err_to_name(err));
            }
            return err;
        }

        esp_err_t AdaSettingsManager::setLedStripBrightness(uint8_t brightness)
        {
            ESP_LOGD(TAG, "Current Brightness: %u, New Brightness: %u", led_strip_brightness_, brightness);
            led_strip_brightness_ = brightness; // Assuming 0-255 range, no specific validation here
            ESP_LOGI(TAG, "Set LED strip brightness to: %u", led_strip_brightness_);

            esp_err_t err = saveLedStripBrightnessToNvs();
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs_handle_);
                if (err == ESP_OK)
                {
                    post_settings_updated_event(APP_EVENT_SETTINGS_CHANGED);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to commit NVS after setting LED brightness: %s", esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to save LED brightness to NVS: %s", esp_err_to_name(err));
            }
            return err;
        }

        esp_err_t AdaSettingsManager::setWakeWordSensitivity(WakeWordSensitivity sensitivity)
        {
            // Validate if sensitivity is within enum range
            if (sensitivity < WakeWordSensitivity::LOW || sensitivity > WakeWordSensitivity::HIGH)
            {
                ESP_LOGE(TAG, "setWakeWordSensitivity: Invalid sensitivity value %u", static_cast<uint8_t>(sensitivity));
                return ESP_ERR_INVALID_ARG;
            }
            wake_word_sensitivity_ = sensitivity;
            ESP_LOGI(TAG, "Set wake word sensitivity to: %u", static_cast<uint8_t>(wake_word_sensitivity_));

            esp_err_t err = saveWakeWordSensitivityToNvs();
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs_handle_);
                if (err == ESP_OK)
                {
                    post_settings_updated_event(APP_EVENT_SETTINGS_CHANGED);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to commit NVS after setting wake word sensitivity: %s", esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to save wake word sensitivity to NVS: %s", esp_err_to_name(err));
            }
            return err;
        }

        esp_err_t AdaSettingsManager::setPowerOnLedSettings(const PowerOnLedSettings &settings)
        {
            // Validate enum values if necessary
            if (settings.mode < PowerOnLedIndicationMode::CONSTANT || settings.mode > PowerOnLedIndicationMode::BLINKING)
            {
                ESP_LOGE(TAG, "setPowerOnLedSettings: Invalid LED mode %u", static_cast<uint8_t>(settings.mode));
                return ESP_ERR_INVALID_ARG;
            }
            if (settings.state < PowerOnLedWorkingState::OFF || settings.state > PowerOnLedWorkingState::ON)
            {
                ESP_LOGE(TAG, "setPowerOnLedSettings: Invalid LED state %u", static_cast<uint8_t>(settings.state));
                return ESP_ERR_INVALID_ARG;
            }

            power_on_led_settings_ = settings;
            ESP_LOGI(TAG, "Set power on LED settings. Mode: %u, State: %u",
                     static_cast<uint8_t>(power_on_led_settings_.mode),
                     static_cast<uint8_t>(power_on_led_settings_.state));

            esp_err_t err = savePowerOnLedSettingsToNvs();
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs_handle_);
                if (err == ESP_OK)
                {
                    post_settings_updated_event(APP_EVENT_SETTINGS_CHANGED);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to commit NVS after setting power LED settings: %s", esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to save power LED settings to NVS: %s", esp_err_to_name(err));
            }
            return err;
        }

    } // namespace settings_manager
} // namespace ada_assistant