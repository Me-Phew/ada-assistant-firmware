#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <cstring>

#include "ada_global_events.hpp"
#include "ada_wifi_manager.hpp"
#include <vector>

namespace ada_assistant
{
    namespace wifi_manager
    {
        static const char *TAG = "ADA_WIFI_MANAGER";

#define WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

        AdaWiFiManager::AdaWiFiManager() : app_event_loop_handle_(nullptr), max_retry_count(CONFIG_ADA_WIFI_MAX_RETRIES), retry_interval(CONFIG_ADA_WIFI_TIMEOUT), retry_count(0)
        {
        }

        AdaWiFiManager::AdaWiFiManager(ada_wifi_manager_config_t config) : app_event_loop_handle_(nullptr), max_retry_count(config.max_retry_count), retry_interval(config.retry_interval), retry_count(0)
        {
        }

        esp_err_t AdaWiFiManager::init(esp_event_loop_handle_t app_event_loop_handle)
        {
            // Initialize the Wi-Fi manager
            ESP_LOGI(TAG, "Initializing Wi-Fi manager");

            app_event_loop_handle_ = app_event_loop_handle;

            ESP_ERROR_CHECK(esp_netif_init());
            esp_netif_create_default_wifi_sta();

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg));

            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                &wifi_event_handler_bridge,
                                                                this,
                                                                &wifi_event_instance_));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                                IP_EVENT_STA_GOT_IP,
                                                                &ip_event_handler_bridge,
                                                                this,
                                                                &ip_event_instance_));

            return ESP_OK;
        }

        std::string AdaWiFiManager::getCurrentNetworkSSID() const
        {
            return current_network_ssid_;
        }

        std::string AdaWiFiManager::getCurrentNetworkPassword() const
        {
            return current_network_password_;
        }

        /*
         * Deinitialize the Wi-Fi manager.
         * This function will clean up any resources used by the Wi-Fi manager.
         * It should be called when the Wi-Fi manager is no longer needed.
         *
         * @return ESP_OK on success, or an error code on failure.
         */
        esp_err_t AdaWiFiManager::deinit()
        {
            ESP_LOGI(TAG, "Deinitializing Wi-Fi manager");

            if (this->wifi_event_instance_)
            {
                esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, this->wifi_event_instance_);
                this->wifi_event_instance_ = nullptr;
            }

            if (this->ip_event_instance_)
            {
                esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, this->ip_event_instance_);
                this->ip_event_instance_ = nullptr;
            }

            return ESP_OK;
        }

        AdaWiFiManager::~AdaWiFiManager()
        {
            ESP_LOGI(TAG, "Destructor called.");
            if (app_event_loop_handle_)
            {
                this->deinit();
            }
        }

        /*
         * Connect to the specified Wi-Fi network.
         * This function will use the stored SSID and password to connect.
         * If the connection fails, it will retry up to max_retry_count times.
         * The retry interval is defined by retry_interval.
         *
         * @param ssid The SSID of the Wi-Fi network to connect to.
         * @param password The password for the Wi-Fi network.
         */
        esp_err_t AdaWiFiManager::connect_to_wifi(const std::string &ssid, const std::string &password)
        {
            current_network_ssid_ = ssid;

            if (current_network_ssid_.empty())
            {
                ESP_LOGE(TAG, "SSID is empty. Cannot connect to Wi-Fi.");
                return ESP_ERR_INVALID_ARG;
            }

            current_network_password_ = password;

            if (current_network_password_.empty())
            {
                ESP_LOGE(TAG, "Password is empty. Cannot connect to Wi-Fi.");
                return ESP_ERR_INVALID_ARG;
            }

            ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: \"%s\"", current_network_ssid_.c_str());

            wifi_config_t wifi_config = {
                .sta = {
                    .ssid = {0},     // Initialize with zeros, then use strncpy below
                    .password = {0}, // Initialize with zeros, then use strncpy below

                    .scan_method = WIFI_FAST_SCAN,            // Default: WIFI_FAST_SCAN, or WIFI_ALL_CHANNEL_SCAN
                    .bssid_set = false,                       // Default: false (don't connect to a specific BSSID)
                    .bssid = {0},                             // If bssid_set is true, fill this
                    .channel = 0,                             // Default: 0 (scan all channels)
                    .listen_interval = 0,                     // Default: 0 (STA will determine based on DTIM) or often 3 for ESP-IDF examples
                    .sort_method = WIFI_CONNECT_AP_BY_SIGNAL, // Default: WIFI_CONNECT_AP_BY_SIGNAL

                    .threshold = {
                        .rssi = -127,                              // Default: -127 (no RSSI threshold if authmode is not WIFI_SCAN_AUTH_MODE_THRESHOLD)
                                                                   // If authmode IS WIFI_SCAN_AUTH_MODE_THRESHOLD, this is the minimum RSSI.
                        .authmode = WIFI_SCAN_AUTH_MODE_THRESHOLD, // Your original setting
                        .rssi_5g_adjustment = 0,                   // No adjustment by default
                    },

                    .pmf_cfg = {
                        .capable = true,   // Default: false, but true is common for WPA3
                        .required = false, // Default: false
                    },

                    .rm_enabled = false,         // Roaming features, default to false
                    .btm_enabled = false,        // Default: false
                    .mbo_enabled = false,        // Default: false (esp_wifi_set_mbo_rssi_threshold to enable)
                    .ft_enabled = false,         // Default: false (802.11r)
                    .owe_enabled = false,        // Default: false (Opportunistic Wireless Encryption)
                    .transition_disable = false, // Default: false

                    .reserved = 0, // Reserved, must be 0

                    .sae_pwe_h2e = WPA3_SAE_PWE_HUNT_AND_PECK, // Or WPA3_SAE_PWE_BOTH. Default for WPA3.
                    .sae_pk_mode = WPA3_SAE_PK_MODE_AUTOMATIC,

                    .failure_retry_cnt = 0, // Default: 0 (depends on IDF version, 0 might mean internal default)
                                            // or a specific number like 3-5. Set to 0 if you handle retries externally.

                    // Wi-Fi 6 (802.11ax) HE STA configuration
                    .he_dcm_set = false,
                    .he_dcm_max_constellation_tx = 0, // Refer to wifi_he_dcm_max_constellation_t
                    .he_dcm_max_constellation_rx = 0, // Refer to wifi_he_dcm_max_constellation_t
                    .he_mcs9_enabled = false,
                    .he_su_beamformee_disabled = false,
                    .he_trig_su_bmforming_feedback_disabled = false,
                    .he_trig_mu_bmforming_partial_feedback_disabled = false, // Your original setting
                    .he_trig_cqi_feedback_disabled = false,
                    .he_reserved = 0, // Reserved for HE, must be 0

                    .sae_h2e_identifier = {0}, // Identifier for SAE H2E, if used.
                },
            };

            snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", current_network_ssid_.c_str());
            snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", current_network_password_.c_str());

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());

            return ESP_OK;
        }

        esp_err_t AdaWiFiManager::connect_to_any_wifi(std::vector<settings_manager::WifiNetwork> wifi_networks)
        {
            // Connect to any available Wi-Fi network from the provided list
            ESP_LOGI(TAG, "Connecting to any Wi-Fi network");

            for (const auto &network : wifi_networks)
            {
                ESP_LOGI(TAG, "Trying to connect to SSID: %s", network.ssid);
                esp_err_t ret = connect_to_wifi(network.ssid, network.password);
                if (ret == ESP_OK)
                {
                    return ret;
                }
            }

            return ESP_FAIL;
        }

        esp_err_t AdaWiFiManager::disconnect_from_wifi()
        {
            // Disconnect from the current Wi-Fi network
            ESP_LOGI(TAG, "Disconnecting from Wi-Fi");

            ESP_ERROR_CHECK(esp_wifi_stop());

            esp_err_t ret = esp_event_post_to(this->app_event_loop_handle_,
                                              ADA_APP_EVENT_BASE,
                                              APP_EVENT_WIFI_DISCONNECTED,
                                              NULL,
                                              0,
                                              portMAX_DELAY);
            return ret;
        }

        esp_err_t AdaWiFiManager::get_wifi_status()
        {
            // Get the current Wi-Fi status
            ESP_LOGI(TAG, "Getting Wi-Fi status");
            return ESP_OK;
        }

        void AdaWiFiManager::wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
        {
            ESP_LOGI(TAG, "Wi-Fi event: %s, event_id: %ld", event_base, event_id);

            switch (event_id)
            {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (retry_count > max_retry_count)
                {
                    ESP_LOGI(TAG, "Connection failed, retry count exceeded");

                    esp_err_t ret = esp_event_post_to(app_event_loop_handle_,
                                                      ADA_APP_EVENT_BASE,
                                                      APP_EVENT_WIFI_CONNECTION_FAILED,
                                                      NULL,
                                                      0,
                                                      portMAX_DELAY);
                    if (ret != ESP_OK)
                    {
                        ESP_LOGE(TAG, "Failed to post APP_EVENT_WIFI_CONNECTION_FAILED: %s", esp_err_to_name(ret));
                    }

                    return;
                }

                esp_wifi_connect();
                retry_count++;
                ESP_LOGI(TAG, "retry to connect to the AP");
                break;
            default:
                ESP_LOGI(TAG, "Unhandled Wi-Fi event: %ld", event_id);
                break;
            }
        }

        void AdaWiFiManager::wifi_event_handler_bridge(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
        {
            // Bridge function to call the actual event handler
            static_cast<AdaWiFiManager *>(arg)->wifi_event_handler(arg, event_base, event_id, event_data);
        }

        void AdaWiFiManager::ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
        {
            ESP_LOGI(TAG, "IP event: %s, event_id: %ld", event_base, event_id);

            switch (event_id)
            {
            case IP_EVENT_STA_GOT_IP:
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                retry_count = 0;

                esp_err_t ret = esp_event_post_to(app_event_loop_handle_,
                                                  ADA_APP_EVENT_BASE,
                                                  APP_EVENT_WIFI_CONNECTED,
                                                  NULL,
                                                  0,
                                                  portMAX_DELAY);
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to post APP_EVENT_WIFI_CONNECTED: %s", esp_err_to_name(ret));
                }

                ESP_LOGI(TAG, "Connected to Wi-Fi");
            }
        }

        void AdaWiFiManager::ip_event_handler_bridge(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
        {
            // Bridge function to call the actual event handler
            static_cast<AdaWiFiManager *>(arg)->ip_event_handler(arg, event_base, event_id, event_data);
        }
    } // namespace wifi_manager
} // namespace ada_assistant
