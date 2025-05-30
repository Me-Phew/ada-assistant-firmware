#ifndef ADA_WIFI_MANAGER
#define ADA_WIFI_MANAGER

#include "esp_err.h"
#include "ada_settings_manager.hpp"

namespace ada_assistant
{
    namespace wifi_manager
    {
        struct ada_wifi_manager_config_t
        {
            int max_retry_count;
            int retry_interval;
        };

        class AdaWiFiManager
        {
        public:
            AdaWiFiManager();

            AdaWiFiManager(ada_wifi_manager_config_t config);

            ~AdaWiFiManager();

            esp_err_t init(esp_event_loop_handle_t app_event_loop_handle_);

            esp_err_t deinit();

            esp_err_t connect_to_wifi(const std::string &ssid, const std::string &password);
            esp_err_t connect_to_any_wifi(std::vector<settings_manager::WifiNetwork> wifi_networks);
            esp_err_t disconnect_from_wifi();

            esp_err_t get_wifi_status();

            std::string getCurrentNetworkSSID() const;

            std::string getCurrentNetworkPassword() const;

        private:
            esp_event_loop_handle_t app_event_loop_handle_;

            std::string current_network_ssid_;
            std::string current_network_password_;

            esp_event_handler_instance_t wifi_event_instance_;
            esp_event_handler_instance_t ip_event_instance_;

            int max_retry_count;
            int retry_interval;

            int retry_count;

            void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

            static void wifi_event_handler_bridge(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

            void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

            static void ip_event_handler_bridge(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
        };

    } // namespace wifi_manager

} // namespace ada_assistant

#endif /* ADA_WIFI_MANAGER */
