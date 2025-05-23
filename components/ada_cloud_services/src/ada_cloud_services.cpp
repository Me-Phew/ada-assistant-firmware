#include "esp_log.h"
#include <string>
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_tls.h"

#include "ada_cloud_services.hpp"

namespace ada_assistant
{
    namespace cloud_services
    {
        static const char *TAG = "ADA_CLOUD_SERVICES";

        esp_err_t AdaCloudServices::http_event_handler_static(esp_http_client_event_t *evt)
        {
            AdaCloudServices *self = (AdaCloudServices *)evt->user_data;
            if (!self)
            {
                ESP_LOGE(TAG, "User data not set in HTTP event handler context");
                // Cannot set self->accumulated_event_error_ here as self is null
                return ESP_FAIL;
            }

            switch (evt->event_id)
            {
            case HTTP_EVENT_ERROR:
                ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
                self->accumulated_event_error_ = ESP_FAIL; // General error
                // More specific error can be extracted if evt->data provides it for this event
                // For example, if *(esp_err_t*)evt->data is the error code:
                // self->accumulated_event_error_ = *(esp_err_t*)evt->data;
                break;
            case HTTP_EVENT_ON_CONNECTED:
                ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
                break;
            case HTTP_EVENT_HEADER_SENT:
                ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
                break;
            case HTTP_EVENT_ON_HEADER:
                ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
                break;
            case HTTP_EVENT_ON_DATA:
                ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
                if (evt->data_len > 0) // self is guaranteed non-null here due to the check at the handler's start
                {
                    // Check if appending this data chunk would exceed the predefined maximum size.
                    // self->current_http_response_body_.length() is size_t
                    // evt->data_len is int
                    // AdaCloudServices::MAX_HTTP_RESPONSE_BODY_SIZE_BYTES is size_t
                    if (self->current_http_response_body_.length() + (size_t)evt->data_len > AdaCloudServices::MAX_HTTP_RESPONSE_BODY_SIZE_BYTES)
                    {
                        ESP_LOGE(TAG, "Response body would exceed maximum allowed size (%zu bytes). Current len: %zu, incoming len: %d. Aborting data accumulation.",
                                 AdaCloudServices::MAX_HTTP_RESPONSE_BODY_SIZE_BYTES,
                                 self->current_http_response_body_.length(),
                                 evt->data_len);
                        self->accumulated_event_error_ = ESP_ERR_NO_MEM; // Or a more specific error like ESP_ERR_HTTP_RESPONSE_TOO_LARGE

                        // Returning ESP_FAIL signals an error to the HTTP client library,
                        // which should then stop processing further data events for this request
                        // and abort the current request.
                        return ESP_FAIL;
                    }

                    // Append data to the response body string.
                    // With -fno-exceptions, if std::string::append fails to allocate memory
                    // internally (e.g., due to severe heap fragmentation despite the size check),
                    // the program will likely call std::terminate() (which usually calls abort()).
                    // The size check above is the primary safeguard against this.
                    self->current_http_response_body_.append((const char *)evt->data, evt->data_len);
                }
                break;
            case HTTP_EVENT_ON_FINISH:
                ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
                // The full response body is now in self->current_http_response_body_.
                // The calling function (e.g., pair_device) will process it after esp_http_client_perform returns.
                break;
            case HTTP_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
                // This event occurs when the connection is closed.
                // It might be due to an error or normal closure.
                // If detailed TLS error information is needed:
                // esp_tls_error_handle_t tls_error_handle = esp_http_client_get_tls_error_handle(evt->client);
                // if (tls_error_handle) {
                //     int mbedtls_err = 0;
                //     esp_err_t err = esp_tls_get_and_clear_last_error(tls_error_handle, &mbedtls_err, NULL);
                //     if (err != ESP_OK) {
                //          ESP_LOGI(TAG, "Last ESP-TLS error: 0x%x", err);
                //          ESP_LOGI(TAG, "Last mbedTLS error: 0x%x", mbedtls_err);
                //          if(self->accumulated_event_error_ == ESP_OK) { // If no other error was recorded
                //              self->accumulated_event_error_ = err;
                //          }
                //     }
                // }
                break;
            // case HTTP_EVENT_REDIRECT: // Available in ESP-IDF v5.0+
            //     ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            //     // Handle redirection if auto-redirect is disabled or specific logic is needed.
            //     // esp_http_client_set_redirection(evt->client); // Example to follow redirection
            //     break;
            default:
                ESP_LOGD(TAG, "Unhandled HTTP_EVENT: %d", evt->event_id);
                break;
            }
            return ESP_OK;
        }

        AdaCloudServices::AdaCloudServices() : app_event_loop_handle_(nullptr),
                                               cloud_server_url_(""),
                                               oem_data_{}, // Value-initialize (zero-initialize for PODs)
                                               firmware_version_(""),
                                               pairing_token_(""),
                                               http_client_(nullptr),
                                               // Initialize new members
                                               current_http_response_body_(""),
                                               accumulated_event_error_(ESP_OK)
        {
            ESP_LOGI(TAG, "Cloud services constructor called.");
        }

        AdaCloudServices::~AdaCloudServices()
        {
            ESP_LOGI(TAG, "Destructor called.");
            // Consider if deinit() should be called here if not already handled by user.
            // For RAII, it might be: if (http_client_) deinit();
            // However, explicit init/deinit is a common pattern in ESP-IDF.
        }

        esp_err_t AdaCloudServices::init(esp_event_loop_handle_t app_event_loop_handle_param, ada_cloud_services_config_t config)
        {
            ESP_LOGI(TAG, "Initializing cloud services.");

            if (http_client_ != nullptr)
            {
                ESP_LOGW(TAG, "Cloud services already initialized. Deinitializing first.");
                return ESP_ERR_INVALID_STATE;
            }

            app_event_loop_handle_ = app_event_loop_handle_param;

            if (config.oem_data.serial_number[0] == '\0')
            {
                ESP_LOGE(TAG, "OEM data serial number is not set.");
                return ESP_ERR_INVALID_ARG;
            }
            oem_data_ = config.oem_data;

            if (config.firmware_version == nullptr || config.firmware_version[0] == '\0')
            {
                ESP_LOGE(TAG, "Firmware version is null or empty.");
                return ESP_ERR_INVALID_ARG;
            }
            firmware_version_ = config.firmware_version;

            if (config.cloud_server_url == nullptr || config.cloud_server_url[0] == '\0')
            {
                ESP_LOGI(TAG, "Cloud server URL is null or empty. Using default: %s", CONFIG_ADA_CLOUD_SERVER_URL);
                cloud_server_url_ = CONFIG_ADA_CLOUD_SERVER_URL;
                if (cloud_server_url_.empty())
                {
                    ESP_LOGE(TAG, "Default cloud server URL (CONFIG_ADA_CLOUD_SERVER_URL) is not set or empty.");
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else
            {
                cloud_server_url_ = config.cloud_server_url;
            }

            if (config.pairing_token.empty())
            {
                ESP_LOGI(TAG, "Pairing token is empty. Assuming device is not paired.");
            }
            else
            {
                ESP_LOGI(TAG, "Device is paired with token: %s", config.pairing_token.c_str());
                pairing_token_ = config.pairing_token;
            }

            // ESP_LOGI(TAG, "Stack size: %d bytes", uxTaskGetStackHighWaterMark(NULL)); // uxTaskGetStackHighWaterMark(NULL) gets current task's HWM
            ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

            esp_http_client_config_t client_config = {}; // Initialize all fields to zero/null
            client_config.timeout_ms = 15000;
            // client_config.crt_bundle_attach = esp_crt_bundle_attach; // TODO HTTPS: Enable for HTTPS

            std::string user_agent_str = "AdaAssistantClient/" + firmware_version_;
            client_config.user_agent = user_agent_str.c_str(); // esp_http_client_init copies this

            client_config.keep_alive_enable = true;
            client_config.url = cloud_server_url_.c_str(); // * Overriden later but must be set to avoid esp_http_client_init failure

            client_config.event_handler = http_event_handler_static;
            client_config.user_data = this; // Pass the current object instance as context
            // client_config.disable_auto_redirect = true; // Set if you want to handle redirects manually via HTTP_EVENT_REDIRECT

            http_client_ = esp_http_client_init(&client_config);

            if (http_client_ == nullptr)
            {
                ESP_LOGE(TAG, "Failed to initialize HTTP client.");
                return ESP_FAIL; // Or ESP_ERR_NO_MEM if that's the likely cause
            }

            return ESP_OK;
        }

        esp_err_t AdaCloudServices::deinit()
        {
            ESP_LOGI(TAG, "Deinitializing cloud services.");

            if (http_client_)
            {
                esp_err_t err = esp_http_client_cleanup(http_client_);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to cleanup HTTP client: %s", esp_err_to_name(err));
                }
                http_client_ = nullptr;
            }
            // Clear string members if desired, though they'll be cleared/reassigned on next init/request
            current_http_response_body_.clear();
            pairing_token_.clear();
            return ESP_OK;
        }

        esp_err_t AdaCloudServices::pair_device(std::string user_id)
        {
            ESP_LOGI(TAG, "Pairing device with user ID: %s", user_id.c_str());

            if (http_client_ == nullptr)
            {
                ESP_LOGE(TAG, "HTTP client is not initialized.");
                return ESP_ERR_INVALID_STATE;
            }

            if (user_id.empty())
            {
                ESP_LOGE(TAG, "User ID is empty.");
                return ESP_ERR_INVALID_ARG;
            }

            if (cloud_server_url_.empty())
            {
                ESP_LOGE(TAG, "Cloud server URL is not configured.");
                return ESP_ERR_INVALID_STATE;
            }

            if (oem_data_.serial_number[0] == '\0')
            {
                ESP_LOGE(TAG, "OEM data serial number is not set.");
                return ESP_ERR_INVALID_STATE;
            }

            if (firmware_version_.empty())
            {
                ESP_LOGE(TAG, "Firmware version is not set.");
                return ESP_ERR_INVALID_STATE;
            }

            // Reset state for this specific request
            current_http_response_body_.clear();
            accumulated_event_error_ = ESP_OK; // Assume success until an event handler flags an error

            std::string endpoint_path = "api/devices/pair"; // Relative path
            std::string full_url;

            // Robust URL construction
            std::string base_url_temp = cloud_server_url_;
            if (!base_url_temp.empty() && base_url_temp.back() == '/')
            {
                base_url_temp.pop_back(); // Remove trailing slash from base if present
            }
            if (!endpoint_path.empty() && endpoint_path.front() == '/')
            {
                full_url = base_url_temp + endpoint_path;
            }
            else
            {
                full_url = base_url_temp + "/" + endpoint_path; // Add leading slash to path if missing
            }

            ESP_LOGI(TAG, "Attempting to pair device. URL: %s", full_url.c_str());
            ESP_LOGI(TAG, "Device Serial: %s, User ID: %s", oem_data_.serial_number, user_id.c_str());

            // 1. Create JSON payload
            cJSON *root = cJSON_CreateObject();
            if (root == NULL)
            {
                ESP_LOGE(TAG, "Failed to create cJSON object for payload.");
                return ESP_ERR_NO_MEM;
            }

            if (cJSON_AddStringToObject(root, "serialNumber", oem_data_.serial_number) == NULL)
            {
                ESP_LOGE(TAG, "Failed to add serialNumber to JSON payload.");
                cJSON_Delete(root);
                return ESP_FAIL; // Or ESP_ERR_NO_MEM
            }

            if (cJSON_AddStringToObject(root, "userId", user_id.c_str()) == NULL)
            {
                ESP_LOGE(TAG, "Failed to add userId to JSON payload.");
                cJSON_Delete(root);
                return ESP_FAIL; // Or ESP_ERR_NO_MEM
            }

            char *json_payload_str = cJSON_PrintUnformatted(root);
            cJSON_Delete(root); // Free cJSON structure, json_payload_str is separate

            if (json_payload_str == NULL)
            {
                ESP_LOGE(TAG, "Failed to print cJSON payload to string.");
                return ESP_ERR_NO_MEM;
            }
            ESP_LOGI(TAG, "JSON Payload: %s", json_payload_str);

            esp_err_t ret = ESP_OK;

            // 2. Configure HTTP client for this specific request
            // Use esp_http_client_set_မethods for clarity on what's being changed on the existing client handle
            ret = esp_http_client_set_url(http_client_, full_url.c_str());
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to set URL for HTTP client: %s", esp_err_to_name(ret));
                cJSON_free(json_payload_str); // Free allocated JSON string
                return ret;
            }

            ret = esp_http_client_set_method(http_client_, HTTP_METHOD_POST);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to set HTTP method: %s", esp_err_to_name(ret));
                cJSON_free(json_payload_str);
                return ret;
            }

            // 3. Set headers and POST data
            esp_http_client_set_header(http_client_, "Content-Type", "application/json");
            esp_http_client_set_header(http_client_, "Accept", "application/json");
            esp_http_client_set_header(http_client_, "Connection", "keep-alive");

            ret = esp_http_client_set_post_field(http_client_, json_payload_str, strlen(json_payload_str));

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to set POST field: %s", esp_err_to_name(ret));
                return ret;
            }

            // 4. Perform the HTTP POST request (blocking call, events are processed within)
            ret = esp_http_client_perform(http_client_);

            cJSON_free(json_payload_str);
            json_payload_str = NULL;

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "HTTP POST request failed (esp_http_client_perform): %s", esp_err_to_name(ret));
                // accumulated_event_error_ might have more details if the error happened during event processing
                if (accumulated_event_error_ != ESP_OK && accumulated_event_error_ != ret)
                {
                    ESP_LOGE(TAG, "Additional error from event handler: %s", esp_err_to_name(accumulated_event_error_));
                }
                return ret; // Primary error from perform
            }

            // Check for errors accumulated in the event handler, even if perform returned ESP_OK
            if (accumulated_event_error_ != ESP_OK)
            {
                ESP_LOGE(TAG, "Error occurred during HTTP event processing: %s", esp_err_to_name(accumulated_event_error_));
                return accumulated_event_error_;
            }

            int status_code = esp_http_client_get_status_code(http_client_);
            ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);

            // The response body is now in current_http_response_body_ (populated by HTTP_EVENT_ON_DATA)

            if (status_code != 200)
            {
                ESP_LOGE(TAG, "Device pairing failed. Server responded with HTTP status %d", status_code);
                if (!current_http_response_body_.empty())
                {
                    ESP_LOGE(TAG, "Server error response: %s", current_http_response_body_.c_str());
                }
                return ESP_FAIL; // Or map HTTP status codes to specific esp_err_t values
            }

            ESP_LOGI(TAG, "Device pairing successful.");
            ESP_LOGD(TAG, "Response body raw: %s", current_http_response_body_.c_str());

            // Parse the response JSON from current_http_response_body_
            cJSON *response_json = cJSON_Parse(current_http_response_body_.c_str());
            if (!response_json)
            {
                ESP_LOGE(TAG, "Failed to parse response JSON. Body: %s", current_http_response_body_.c_str());
                return ESP_FAIL; // Or ESP_ERR_INVALID_RESPONSE
            }

            cJSON *pairing_token_json = cJSON_GetObjectItem(response_json, "token");
            if (!cJSON_IsString(pairing_token_json) || (pairing_token_json->valuestring == NULL))
            {
                ESP_LOGE(TAG, "Failed to get 'token' string from response JSON.");
                cJSON_Delete(response_json);
                return ESP_FAIL; // Or ESP_ERR_INVALID_RESPONSE
            }

            pairing_token_ = pairing_token_json->valuestring;
            ESP_LOGI(TAG, "Pairing token received: %s", pairing_token_.c_str());

            cJSON_Delete(response_json); // Free cJSON structure for response

            // Post event about successful pairing
            esp_err_t post_ret = esp_event_post_to(app_event_loop_handle_,
                                                   ADA_APP_EVENT_BASE,
                                                   APP_EVENT_PAIRING_COMPLETED,
                                                   (void *)pairing_token_.c_str(),
                                                   pairing_token_.length() + 1, // Include null terminator
                                                   portMAX_DELAY);

            if (post_ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to post pairing completed event: %s", esp_err_to_name(post_ret));
                // Continue, as pairing itself was successful. The event post failure is a separate issue.
            }
            else
            {
                ESP_LOGI(TAG, "Pairing completed event posted successfully.");
            }

            return ESP_OK;
        }

        esp_err_t AdaCloudServices::connect_to_gateway()
        {
            ESP_LOGI(TAG, "Connecting to gateway with server URL: %s", cloud_server_url_.c_str());
            // TODO: Implement gateway connection logic (e.g., WebSocket, MQTT)
            // This would likely involve a different client or protocol.
            // If it's HTTP-based, it would follow a similar pattern to pair_device.
            return ESP_OK; // Placeholder
        }

        esp_err_t AdaCloudServices::disconnect_from_gateway(disconnection_reason_t reason)
        {
            ESP_LOGI(TAG, "Disconnecting from gateway (reason: %d) with server URL: %s", reason, cloud_server_url_.c_str());
            // TODO: Implement gateway disconnection logic
            return ESP_OK; // Placeholder
        }

        std::string AdaCloudServices::getPairingToken() const
        {
            return pairing_token_;
        }

    } // namespace cloud_services
} // namespace ada_assistant