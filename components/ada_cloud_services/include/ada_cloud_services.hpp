#ifndef ADA_CLOUD_SERVICES
#define ADA_CLOUD_SERVICES

#include "esp_err.h"
#include "esp_event.h"
#include <string>
#include "esp_http_client.h"

#include "ada_oem_data.h"
#include "ada_global_events.hpp"
#include "ada_settings_manager.hpp"

namespace ada_assistant
{
    namespace cloud_services
    {
        enum disconnection_reason_t
        {
            DISCONNECTION_REASON_SHUTDOWN = 0,
            DISCONNECTION_REASON_ERROR,
        };

        typedef struct
        {
            const char *cloud_server_url;
            ada_oem_data_t oem_data;
            const char *firmware_version;
            std::string pairing_token;
        } ada_cloud_services_config_t;

        typedef struct
        {
            bool is_playback_start_request;
            // TODO Consider using dynamic allocation
            // These strings are big for stack allocation and URLS could be longer than 256 characters.
            // For now, we use fixed-size buffers to avoid dynamic memory allocation.
            char response_url[256];
            char playback_audio_url[256];
        } event_command_processing_finished_data_t;

        class AdaCloudServices
        {
        public:
            AdaCloudServices();
            ~AdaCloudServices();

            esp_err_t init(esp_event_loop_handle_t app_event_loop_handle_, ada_cloud_services_config_t config);
            esp_err_t deinit();

            esp_err_t pair_device(std::string user_id);

            esp_err_t connect_to_gateway();
            esp_err_t disconnect_from_gateway(disconnection_reason_t reason);

            std::string getPairingToken() const;

            esp_err_t sendAudio(const int16_t *audio_data, size_t audio_data_bytes, int sample_rate, int bits_per_sample);

        private:
            esp_event_loop_handle_t app_event_loop_handle_;

            std::string cloud_server_url_;
            ada_oem_data_t oem_data_;
            std::string firmware_version_;
            std::string pairing_token_;

            esp_http_client_handle_t http_client_;

            // --- New members for event-driven HTTP handling ---
            std::string current_http_response_body_;
            esp_err_t accumulated_event_error_;
            // --- End new members ---

            // Define the maximum size for the HTTP response body
            // This helps prevent out-of-memory issues when exceptions are disabled.
            // C++17: static inline const size_t MAX_HTTP_RESPONSE_BODY_SIZE_BYTES = 10 * 1024; // 10KB
            // For C++11/14, a static const member initialized here is fine if not ODR-used (address taken).
            // If you encounter linker errors, define it in the .cpp file:
            // header: static const size_t MAX_HTTP_RESPONSE_BODY_SIZE_BYTES;
            // cpp: const size_t AdaCloudServices::MAX_HTTP_RESPONSE_BODY_SIZE_BYTES = 10 * 1024;
            static const size_t MAX_HTTP_RESPONSE_BODY_SIZE_BYTES = 10 * 1024; // 10KB example
            static const size_t response_timeout_ms_ = 90000;

            // --- Static event handler declaration ---
            static esp_err_t http_event_handler_static(esp_http_client_event_t *evt);
            // --- End static event handler declaration ---

            static std::string generate_form_boundary();
        };

    } // namespace cloud_services
} // namespace ada_assistant

#endif /* ADA_CLOUD_SERVICES */