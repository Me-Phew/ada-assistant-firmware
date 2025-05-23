#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include <cstring>
#include "cJSON.h"

#include "ada_bluetooth_manager.hpp"
#include "ada_global_events.hpp"

namespace ada_assistant
{
    namespace bluetooth_manager
    {
        static const char *TAG = "ADA_BT_MANAGER";

#define DEVICE_NAME CONFIG_ADA_BLUETOOTH_DEVICE_NAME

        static AdaBluetoothManager *g_ble_manager_instance = nullptr;

        static inline esp_bt_uuid_t create_full_uuid(uint16_t alias)
        {
            esp_bt_uuid_t full_uuid;
            full_uuid.len = ESP_UUID_LEN_128;
            memcpy(full_uuid.uuid.uuid128, ADA_VENDOR_BASE_UUID, ESP_UUID_LEN_128);

            // Place alias in the last two bytes of the base UUID for uniqueness
            // Base: ... F4 27 96 4E. For ADA0: ... ADA0 96 4E -> ... F4 27 ADA0 (if bytes 12,13 are used)
            // Original code used bytes 14, 15 (0-indexed). Let's stick to that.
            // ADA_VENDOR_BASE_UUID[12], ADA_VENDOR_BASE_UUID[13] are 0xf4, 0x27
            // Using index 14 and 15 for alias like original for:
            // ... 4E9627F4-C152-4957-BC14-6E24A10ACC1F (reversed)
            //   1f cc 0a a1 24 6e 14 bc 57 49 52 c1 f4 27 96 4e
            //                                          LSB MSB
            // So alias 0xADA0 -> uuid128[14] = 0xA0, uuid128[15] = 0xAD
            full_uuid.uuid.uuid128[14] = (alias >> 0) & 0xFF; // LSB of alias
            full_uuid.uuid.uuid128[15] = (alias >> 8) & 0xFF; // MSB of alias
            return full_uuid;
        }

// Service UUID
#define GET_ADA_SETUP_SERVICE_UUID() create_full_uuid(ADA_SETUP_SERVICE_ALIAS)
// Characteristic UUIDs
#define GET_ADA_CHAR_SSID_UUID() create_full_uuid(ADA_CHAR_SSID_ALIAS)
#define GET_ADA_CHAR_PASSWORD_UUID() create_full_uuid(ADA_CHAR_PASSWORD_ALIAS)
#define GET_ADA_CHAR_USER_ID_UUID() create_full_uuid(ADA_CHAR_USER_ID_ALIAS)
#define GET_ADA_CHAR_STATUS_UUID() create_full_uuid(ADA_CHAR_STATUS_ALIAS)
#define GET_ADA_CHAR_CONTROL_UUID() create_full_uuid(ADA_CHAR_CONTROL_ALIAS)

#define ADA_GATTS_APP_ID 0

        // GATT Service UUID
        static const esp_bt_uuid_t GATTS_SERVICE_UUID_SETUP = GET_ADA_SETUP_SERVICE_UUID();
        // GATT Characteristic UUIDs
        static const esp_bt_uuid_t GATTS_CHAR_UUID_SSID = GET_ADA_CHAR_SSID_UUID();
        static const esp_bt_uuid_t GATTS_CHAR_UUID_PASSWORD = GET_ADA_CHAR_PASSWORD_UUID();
        static const esp_bt_uuid_t GATTS_CHAR_UUID_USER_ID = GET_ADA_CHAR_USER_ID_UUID();
        static const esp_bt_uuid_t GATTS_CHAR_UUID_STATUS = GET_ADA_CHAR_STATUS_UUID();
        static const esp_bt_uuid_t GATTS_CHAR_UUID_CONTROL = GET_ADA_CHAR_CONTROL_UUID();

        // Characteristic properties (used in the ESP-IDF specific table format)
        static const esp_gatt_char_prop_t SSID_CHAR_PROPERTY = ESP_GATT_CHAR_PROP_BIT_WRITE;
        static const esp_gatt_char_prop_t PASSWORD_CHAR_PROPERTY = ESP_GATT_CHAR_PROP_BIT_WRITE;
        static const esp_gatt_char_prop_t USER_ID_CHAR_PROPERTY = ESP_GATT_CHAR_PROP_BIT_WRITE;
        static const esp_gatt_char_prop_t STATUS_CHAR_PROPERTY = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        static const esp_gatt_char_prop_t CONTROL_CHAR_PROPERTY = ESP_GATT_CHAR_PROP_BIT_WRITE;

        // Default characteristic values (can be empty or placeholders)
        static uint8_t char_value_ssid[1] = {0x00};                        // Max len defined by APP_MAX_SSID_LEN
        static uint8_t char_value_password[1] = {0x00};                    // Max len defined by MAX_PASSWORD_LEN
        static uint8_t char_value_user_id[1] = {0x00};                     // Max len defined by MAX_USER_ID_LEN
        static uint8_t char_value_status[MAX_STATUS_MESSAGE_LEN] = {0x00}; // Buffer for current status JSON
        static uint8_t char_value_control[1] = {0x00};                     // For control point commands

        // Standard UUID for Client Characteristic Configuration Descriptor (CCCD)
        static const uint16_t GATTS_CHAR_CFG_UUID = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        // Initial value for the Status Characteristic's CCCD (0x0000: notifications/indications disabled)
        static uint16_t s_char_status_ccc_val = 0x0000;

        static const uint16_t GATTS_SERVICE_UUID_PRIMARY = ESP_GATT_UUID_PRI_SERVICE; // 0x2800

        // Define standard characteristic declaration UUID
        static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
        static const uint16_t client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

        // Define property values
        static const uint8_t char_prop_read_write = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE;
        static const uint8_t char_prop_read = ESP_GATT_CHAR_PROP_BIT_READ;
        static const uint8_t char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE;

        static const uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        static const uint8_t char_prop_write_no_resp = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;

        // GATT Attribute Table Definition
        static const esp_gatts_attr_db_t gatt_db[ADA_IDX_NB] = {
            // Service Declaration
            [IDX_SVC] = {
                {ESP_GATT_AUTO_RSP},
                {ESP_UUID_LEN_16,
                 (uint8_t *)&GATTS_SERVICE_UUID_PRIMARY,
                 ESP_GATT_PERM_READ,
                 GATTS_SERVICE_UUID_SETUP.len,
                 GATTS_SERVICE_UUID_SETUP.len,
                 (uint8_t *)&GATTS_SERVICE_UUID_SETUP.uuid}},

            /* SSID Characteristic Declaration */
            [IDX_CHAR_SSID] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_read_write}},

            /* SSID Characteristic Value */
            [IDX_CHAR_VAL_SSID] = {{ESP_GATT_AUTO_RSP}, {GATTS_CHAR_UUID_SSID.len, (uint8_t *)&GATTS_CHAR_UUID_SSID.uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE | ESP_GATT_PERM_WRITE_ENCRYPTED, MAX_SSID_LEN, sizeof(char_value_ssid), (uint8_t *)char_value_ssid}},

            /* Password Characteristic Declaration */
            [IDX_CHAR_PASSWORD] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_read_write}},

            /* Password Characteristic Value */
            [IDX_CHAR_VAL_PASSWORD] = {{ESP_GATT_AUTO_RSP}, {GATTS_CHAR_UUID_PASSWORD.len, (uint8_t *)&GATTS_CHAR_UUID_PASSWORD.uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE | ESP_GATT_PERM_WRITE_ENCRYPTED, MAX_PASSWORD_LEN, sizeof(char_value_password), (uint8_t *)char_value_password}},

            /* User ID Characteristic Declaration */
            [IDX_CHAR_USER_ID] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_read_write}},

            /* User ID Characteristic Value */
            [IDX_CHAR_VAL_USER_ID] = {{ESP_GATT_AUTO_RSP}, {GATTS_CHAR_UUID_USER_ID.len, (uint8_t *)&GATTS_CHAR_UUID_USER_ID.uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE | ESP_GATT_PERM_WRITE_ENCRYPTED, MAX_USER_ID_LEN, sizeof(char_value_user_id), (uint8_t *)char_value_user_id}},

            /* Status Characteristic Declaration */
            [IDX_CHAR_STATUS] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_read_notify}},

            /* Status Characteristic Value */
            [IDX_CHAR_VAL_STATUS] = {{ESP_GATT_AUTO_RSP}, {GATTS_CHAR_UUID_STATUS.len, (uint8_t *)&GATTS_CHAR_UUID_STATUS.uuid, ESP_GATT_PERM_READ, MAX_STATUS_MESSAGE_LEN, sizeof(char_value_status), (uint8_t *)char_value_status}},

            /* Status Client Characteristic Configuration Descriptor (CCCD) */
            [IDX_CHAR_CFG_STATUS] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t), sizeof(s_char_status_ccc_val), (uint8_t *)&s_char_status_ccc_val}},

            /* Control Characteristic Declaration */
            [IDX_CHAR_CONTROL] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_write}},

            /* Control Characteristic Value */
            [IDX_CHAR_VAL_CONTROL] = {{ESP_GATT_AUTO_RSP}, {GATTS_CHAR_UUID_CONTROL.len, (uint8_t *)&GATTS_CHAR_UUID_CONTROL.uuid, ESP_GATT_PERM_WRITE | ESP_GATT_PERM_WRITE_ENCRYPTED, sizeof(char_value_control), sizeof(char_value_control), (uint8_t *)char_value_control}}};

        AdaBluetoothManager::AdaBluetoothManager() : m_app_event_loop_handle(nullptr),
                                                     m_is_ble_initialized(false),
                                                     m_is_advertising(false),
                                                     m_is_app_gatt_connected(false),
                                                     m_gatts_if(ESP_GATT_IF_NONE),
                                                     m_conn_id(0xFFFF), // Invalid connection ID
                                                     m_ssid_received(false),
                                                     m_password_received(false),
                                                     m_user_id_received(false),
                                                     m_ble_op_mutex(nullptr)
        {
            g_ble_manager_instance = this;
            memset(m_gatts_handle_table, 0, sizeof(m_gatts_handle_table));
            memset(&m_remote_bda, 0, sizeof(m_remote_bda));

            // Default advertising parameters
            m_adv_params = {
                .adv_int_min = 0x20,      // Min advertisement interval (0x20 * 0.625ms = 20ms)
                .adv_int_max = 0x40,      // Max advertisement interval (0x40 * 0.625ms = 40ms)
                .adv_type = ADV_TYPE_IND, // Undirected connectable advertisement
                .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .peer_addr = {0},
                .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .channel_map = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            };
        }

        AdaBluetoothManager::~AdaBluetoothManager()
        {
            ESP_LOGI(TAG, "Destructor called.");
            if (m_is_ble_initialized)
            {
                // Ensure proper cleanup if BLE was active
                finalize_setup_and_disable_ble(); // This already calls deinit_ble_stack
            }
            if (m_ble_op_mutex)
            {
                vSemaphoreDelete(m_ble_op_mutex);
                m_ble_op_mutex = nullptr;
            }
            g_ble_manager_instance = nullptr;
        }

        esp_err_t AdaBluetoothManager::init(esp_event_loop_handle_t app_event_loop_handle)
        {
            if (m_is_ble_initialized)
            {
                ESP_LOGW(TAG, "BLE already initialized.");
                return ESP_OK;
            }
            m_app_event_loop_handle = app_event_loop_handle;
            esp_err_t ret;

            ESP_LOGI(TAG, "Initializing BLE Manager...");
            if (!m_ble_op_mutex)
            { // Create mutex if not already created (e.g. if init is called multiple times after deinit)
                m_ble_op_mutex = xSemaphoreCreateMutex();
            }
            if (!m_ble_op_mutex)
            {
                ESP_LOGE(TAG, "Failed to create BLE op mutex");
                return ESP_FAIL;
            }

            ret = init_ble_stack();
            if (ret != ESP_OK)
            {
                // Mutex cleanup might be needed if other resources were allocated.
                // vSemaphoreDelete(m_ble_op_mutex); m_ble_op_mutex = nullptr; // if this is a fatal error path
                return ret;
            }

            ret = esp_ble_gatts_register_callback(ble_gatts_event_handler_bridge);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "GATTS register callback failed: %s", esp_err_to_name(ret));
                deinit_ble_stack(); // Clean up BLE stack
                return ret;
            }

            ret = esp_ble_gap_register_callback(ble_gap_event_handler_bridge);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
                esp_ble_gatts_register_callback(NULL); // Unregister GATTS callback
                deinit_ble_stack();                    // Clean up BLE stack
                return ret;
            }

            ret = esp_ble_gatts_app_register(ADA_GATTS_APP_ID);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(ret));
                esp_ble_gap_register_callback(NULL);   // Unregister GAP callback
                esp_ble_gatts_register_callback(NULL); // Unregister GATTS callback
                deinit_ble_stack();                    // Clean up BLE stack
                return ret;
            }

            // <<< --- ADD SECURITY PARAMETER SETUP HERE --- >>>

            ESP_LOGI(TAG, "Setting GAP Security Parameters for encrypted connection (Just Works)...");

            // 1. Authentication Requirements:
            //    - ESP_LE_AUTH_REQ_SC_BOND: Secure Connections, No MITM, Bonding
            //    - ESP_LE_AUTH_REQ_BOND: Legacy Pairing, No MITM, Bonding
            //    - ESP_LE_AUTH_NO_BOND: No Bonding (keys not stored)
            //    For "Just Works" with encryption and saving the bond:
            esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND; // Prioritize Secure Connections
            // If SC fails or peer doesn't support, it might fall back to legacy if not strictly SC_ONLY.
            // esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND; // For legacy pairing (if SC is an issue)
            ret = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Set AUTHEN_REQ_MODE failed: %s", esp_err_to_name(ret));
                // Decide if this is fatal, for now we log and continue
            }

            // 2. I/O Capabilities:
            //    For "Just Works" (no display, no keyboard, no MITM confirmation step):
            esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
            ret = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Set IOCAP_MODE failed: %s", esp_err_to_name(ret));
            }

            // 3. Key Size: (Usually 16 for robust security)
            uint8_t key_size = 16; // Max is 16
            ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Set MAX_KEY_SIZE failed: %s", esp_err_to_name(ret));
            }

            // 4. Initiator Keys to Distribute (ESP32 as peripheral/slave initiates less often, but good to set)
            //    ESP_BLE_ENC_KEY_MASK: LTK (Long Term Key for encryption)
            //    ESP_BLE_ID_KEY_MASK: IRK (Identity Resolving Key for private addresses)
            uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
            ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Set SET_INIT_KEY failed: %s", esp_err_to_name(ret));
            }

            // 5. Responder Keys to Distribute (ESP32 as peripheral/slave acts as responder)
            uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
            ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Set SET_RSP_KEY failed: %s", esp_err_to_name(ret));
            }
            // <<< --- END OF SECURITY PARAMETER SETUP --- >>>

            // Set MTU size - ESP IDF default is 23. Max is 517.
            // For larger characteristic values (like status message), a larger MTU is beneficial.
            // The client also needs to request an MTU exchange.
            // This call sets the *local* max MTU the server can support.
            // ret = esp_ble_gatt_set_local_mtu(200);
            // if (ret != ESP_OK)
            // {
            //     ESP_LOGE(TAG, "Set local MTU failed: %s", esp_err_to_name(ret));
            //     // Not necessarily fatal, can continue with default MTU, but notifications might be fragmented.
            // }

            m_is_ble_initialized = true; // Mark as initialized only after all steps succeed
            ESP_LOGI(TAG, "BLE stack initialized successfully.");
            return ESP_OK;
        }

        esp_err_t AdaBluetoothManager::init_ble_stack()
        {
            esp_err_t ret;
            ESP_LOGI(TAG, "Initializing BLE Controller and Bluedroid (for BLE).");

            // esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT); // Release classic BT memory if not used

            esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
            // bt_cfg.bluetooth_mode = ESP_BT_MODE_BLE; // This is default in BT_CONTROLLER_INIT_CONFIG_DEFAULT for ESP32

            ret = esp_bt_controller_init(&bt_cfg);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Initialize BT controller failed: %s", esp_err_to_name(ret));
                return ret;
            }

            ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Enable BT controller failed: %s", esp_err_to_name(ret));
                // esp_bt_controller_deinit(); // Clean up if enable fails
                return ret;
            }

            ret = esp_bluedroid_init();
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Initialize Bluedroid failed: %s", esp_err_to_name(ret));
                // esp_bt_controller_disable(); esp_bt_controller_deinit(); // Clean up
                return ret;
            }

            ret = esp_bluedroid_enable();
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Enable Bluedroid failed: %s", esp_err_to_name(ret));
                // esp_bluedroid_deinit(); esp_bt_controller_disable(); esp_bt_controller_deinit(); // Clean up
                return ret;
            }
            return ESP_OK;
        }

        esp_err_t AdaBluetoothManager::start_setup_mode()
        {
            if (!m_is_ble_initialized)
            {
                ESP_LOGE(TAG, "BLE not initialized. Call init() first.");
                return ESP_ERR_INVALID_STATE;
            }
            if (m_is_advertising)
            {
                ESP_LOGW(TAG, "Already advertising.");
                return ESP_OK;
            }

            m_adv_device_name = DEVICE_NAME;
            ESP_LOGI(TAG, "Starting BLE setup mode. Advertising as: %s", m_adv_device_name.c_str());

            esp_err_t ret = esp_ble_gap_set_device_name(m_adv_device_name.c_str());
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Set device name failed: %s", esp_err_to_name(ret));
                // return ret; // Not fatal, can continue with default name or no name in adv
            }

            // Configure advertising data
            m_adv_data = {}; // Zero initialize
            m_adv_data.set_scan_rsp = false;
            m_adv_data.include_name = false;
            m_adv_data.include_txpower = false;
            // m_adv_data.min_interval = 0x0006; // Recommended values for fast connection: 0x0020 (20ms) to 0x0040 (40ms)
            // m_adv_data.max_interval = 0x000C; // Values from original code, very short. Let's use slightly longer ones.
            //                                   // e.g. min_interval = 0x20 (20*1.25ms=25ms), max_interval=0x40 (40*1.25ms=50ms)
            //                                   // For now, stick to original example values if they worked.
            m_adv_data.appearance = 0x00; // Generic
            m_adv_data.manufacturer_len = 0;
            m_adv_data.p_manufacturer_data = NULL;
            m_adv_data.service_data_len = 0;
            m_adv_data.p_service_data = NULL;

            // Advertise our main service UUID
            m_adv_data.service_uuid_len = GATTS_SERVICE_UUID_SETUP.len;
            m_adv_data.p_service_uuid = (uint8_t *)&GATTS_SERVICE_UUID_SETUP.uuid;
            m_adv_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);

            ret = esp_ble_gap_config_adv_data(&m_adv_data);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Config adv data failed: %s", esp_err_to_name(ret));
                return ret;
            }

            // Scan response data (optional)
            m_scan_rsp_data = {}; // Zero initialize
            m_scan_rsp_data.set_scan_rsp = true;
            m_scan_rsp_data.include_name = true;
            m_scan_rsp_data.include_txpower = true;
            // Optionally add more service UUIDs or manufacturer data to scan response
            // m_scan_rsp_data.service_uuid_len = ...
            // m_scan_rsp_data.p_service_uuid = ...

            ret = esp_ble_gap_config_adv_data(&m_scan_rsp_data); // Configures scan response
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Config scan response data failed: %s", esp_err_to_name(ret));
                // Not fatal, can continue without scan response
            }

            // Advertising parameters are already set in constructor, can be updated here if needed
            // m_adv_params.adv_int_min = ...

            ret = esp_ble_gap_start_advertising(&m_adv_params);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Start advertising failed: %s", esp_err_to_name(ret));
                return ret;
            }
            // m_is_advertising will be set to true in ESP_GAP_BLE_ADV_START_COMPLETE_EVT if successful
            ESP_LOGI(TAG, "BLE Advertising start initiated.");
            // Initial status update might be better sent after ESP_GAP_BLE_ADV_START_COMPLETE_EVT
            // send_status_update_to_app(SETUP_STATUS_WAITING_FOR_APP_CONNECTION, "Device is discoverable.");
            return ESP_OK;
        }

        esp_err_t AdaBluetoothManager::stop_setup_mode()
        {
            if (m_is_advertising)
            {
                esp_err_t ret = esp_ble_gap_stop_advertising();
                if (ret == ESP_OK)
                {
                    ESP_LOGI(TAG, "BLE Advertising stop initiated.");
                    // m_is_advertising will be set to false in ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to stop advertising: %s", esp_err_to_name(ret));
                    m_is_advertising = false; // Force state if API call fails critically
                }

                return ret;
            }

            if (m_is_app_gatt_connected && m_conn_id != 0xFFFF)
            {
                ESP_LOGI(TAG, "Disconnecting connected GATT client (conn_id: %d).", m_conn_id);
                esp_ble_gap_disconnect(m_remote_bda); // Disconnect the specific client
                // Connection state (m_is_app_gatt_connected, m_conn_id) updated in ESP_GATTS_DISCONNECT_EVT
            }

            return ESP_OK;
        }

        void AdaBluetoothManager::ble_gap_event_handler_bridge(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
        {
            if (g_ble_manager_instance)
            {
                g_ble_manager_instance->ble_gap_event_handler(event, param);
            }
        }

        void AdaBluetoothManager::ble_gatts_event_handler_bridge(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
        {
            if (g_ble_manager_instance)
            {
                g_ble_manager_instance->ble_gatts_event_handler(event, gatts_if, param);
            }
        }

        void AdaBluetoothManager::ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
        {
            switch (event)
            {
            case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
                ESP_LOGI(TAG, "GAP ADV_DATA_SET_COMPLETE, status: %d", param->adv_data_cmpl.status);
                // If scan response was also set, this event might fire twice.
                // Advertising is typically started after both main adv data and scan rsp data are set.
                // (Current code starts advertising after main adv data, assuming scan rsp is optional or set quickly)
                break;
            case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
                ESP_LOGI(TAG, "GAP SCAN_RSP_DATA_SET_COMPLETE, status: %d", param->scan_rsp_data_cmpl.status);
                break;
            case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
                ESP_LOGI(TAG, "GAP ADV_START_COMPLETE, status: %d", param->adv_start_cmpl.status);
                if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
                {
                    m_is_advertising = true;
                    ESP_LOGI(TAG, "Advertising started successfully.");
                    send_status_update_to_app(SETUP_STATUS_WAITING_FOR_APP_CONNECTION, "Device is discoverable.");
                }
                else
                {
                    m_is_advertising = false;
                    ESP_LOGE(TAG, "Advertising start failed");
                    // Potentially post an error event to the application
                }
                break;
            case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
                ESP_LOGI(TAG, "GAP ADV_STOP_COMPLETE, status: %d", param->adv_stop_cmpl.status);
                m_is_advertising = false;
                if (param->adv_stop_cmpl.status == ESP_BT_STATUS_SUCCESS)
                {
                    ESP_LOGI(TAG, "Advertising stopped successfully.");
                }
                else
                {
                    ESP_LOGE(TAG, "Advertising stop failed.");
                }
                break;
            case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
                ESP_LOGI(TAG, "GAP UPDATE_CONN_PARAMS_EVT, status: %d, min_int: %#x, max_int: %#x, conn_int: %#x, latency: %d, timeout: %d",
                         param->update_conn_params.status,
                         param->update_conn_params.min_int,
                         param->update_conn_params.max_int,
                         param->update_conn_params.conn_int,
                         param->update_conn_params.latency,
                         param->update_conn_params.timeout);
                break;
            case ESP_GAP_BLE_SEC_REQ_EVT: // Security request
                ESP_LOGI(TAG, "GAP SEC_REQ_EVT: Sending security response (accepting pairing)");
                esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
                break;
            case ESP_GAP_BLE_NC_REQ_EVT: // Numeric comparison request (for Secure Connections pairing)
                ESP_LOGI(TAG, "GAP NC_REQ_EVT, passkey: %lu", (unsigned long)param->ble_security.key_notif.passkey);
                // For "Just Works" or if a display is not available for numeric comparison, auto-confirm.
                // If you have a display, show the passkey and ask user to confirm on both devices.
                esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
                ESP_LOGI(TAG, "Numeric comparison confirmed.");
                break;
            case ESP_GAP_BLE_KEY_EVT: // Local or peer key received
                ESP_LOGI(TAG, "GAP KEY_EVT, key type: 0x%02x", param->ble_security.ble_key.key_type);
                // Handle keys if needed, e.g. for bonding persistence
                break;
            case ESP_GAP_BLE_AUTH_CMPL_EVT: // Authentication complete
                ESP_LOGI(TAG, "GAP AUTH_CMPL_EVT, status: %s, addr: %02x:%02x:%02x:%02x:%02x:%02x",
                         param->ble_security.auth_cmpl.success ? "Success" : "Fail",
                         param->ble_security.auth_cmpl.bd_addr[0], param->ble_security.auth_cmpl.bd_addr[1],
                         param->ble_security.auth_cmpl.bd_addr[2], param->ble_security.auth_cmpl.bd_addr[3],
                         param->ble_security.auth_cmpl.bd_addr[4], param->ble_security.auth_cmpl.bd_addr[5]);
                ESP_LOGI(TAG, "Bonded: %s, Addr Type: %d, Key Type: 0x%02x, Dev type: %d",
                         param->ble_security.auth_cmpl.key_present ? "Yes" : "No",
                         param->ble_security.auth_cmpl.addr_type,
                         param->ble_security.auth_cmpl.key_type,
                         param->ble_security.auth_cmpl.dev_type);
                if (param->ble_security.auth_cmpl.success)
                {
                    ESP_LOGI(TAG, "Pairing/Bonding successful.");
                    // If bonding enabled & successful, keys are stored by Bluedroid.
                    esp_event_post_to(m_app_event_loop_handle, ADA_APP_EVENT_BASE, APP_EVENT_BLE_AUTH_CMPL, NULL, 0, portMAX_DELAY);
                }
                else
                {
                    ESP_LOGE(TAG, "Pairing/Bonding failed, reason: 0x%x", param->ble_security.auth_cmpl.fail_reason);
                    // Handle pairing failure, e.g., disconnect.
                    esp_ble_gap_disconnect(param->ble_security.auth_cmpl.bd_addr);
                }
                break;

            default:
                ESP_LOGD(TAG, "Unhandled GAP Event: %d", event);
                break;
            }
        }

        void AdaBluetoothManager::ble_gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
        {
            ESP_LOGD(TAG, "GATTS Event: %s, IF: %d", esp_err_to_name(event), gatts_if); // Using esp_err_to_name for event numbers can be informative

            switch (event)
            {
            case ESP_GATTS_REG_EVT:
                ESP_LOGI(TAG, "GATTS_REG_EVT, status: %s, app_id: %d, gatts_if: %d",
                         esp_err_to_name(param->reg.status), param->reg.app_id, gatts_if);
                if (param->reg.status == ESP_GATT_OK)
                {
                    m_gatts_if = gatts_if;
                    // The service and its characteristics are defined in `gatt_db`.
                    // `esp_ble_gatts_create_attr_tab` will create all of them, including the service itself.
                    //
                    // If you were to create the service programmatically (instead of using a table),
                    // you would call `esp_ble_gatts_create_service()` here, like so:
                    //
                    // esp_ble_srvc_id_t service_id;
                    // service_id.is_primary = true;
                    // service_id.id.inst_id = 0;
                    // service_id.id.uuid = GATTS_SERVICE_UUID_SETUP; // Your service UUID
                    // esp_ble_gatts_create_service(gatts_if, &service_id, ADA_IDX_NB /* Total handles needed */);
                    //
                    // This would then be followed by calls to `esp_ble_gatts_add_char`, `esp_ble_gatts_add_char_descr`
                    // in subsequent event handlers (ESP_GATTS_CREATE_SRVC_EVT, ESP_GATTS_ADD_CHAR_EVT, etc.).
                    //
                    // Since `gatt_db` is used, we call `esp_ble_gatts_create_attr_tab` instead:
                    esp_err_t create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, ADA_IDX_NB, ADA_GATTS_APP_ID);
                    if (create_attr_ret != ESP_OK)
                    {
                        ESP_LOGE(TAG, "Create attribute table failed, error code = %s", esp_err_to_name(create_attr_ret));
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "GATTS Register app failed, app_id %04x, status %s", param->reg.app_id, esp_err_to_name(param->reg.status));
                }
                break;

            case ESP_GATTS_CREAT_ATTR_TAB_EVT:
                ESP_LOGI(TAG, "GATTS_CREAT_ATTR_TAB_EVT, status: %s, num_handle: %d, app_id: %d",
                         esp_err_to_name(param->add_attr_tab.status), param->add_attr_tab.num_handle, param->add_attr_tab.svc_inst_id); // svc_inst_id is app_id
                if (param->add_attr_tab.status == ESP_GATT_OK)
                {
                    if (param->add_attr_tab.num_handle == ADA_IDX_NB)
                    {
                        memcpy(m_gatts_handle_table, param->add_attr_tab.handles, sizeof(m_gatts_handle_table));
                        ESP_LOGI(TAG, "Attribute table created, handles stored. Starting service (handle: %d)...", m_gatts_handle_table[IDX_SVC]);
                        esp_err_t start_svc_ret = esp_ble_gatts_start_service(m_gatts_handle_table[IDX_SVC]);
                        if (start_svc_ret != ESP_OK)
                        {
                            ESP_LOGE(TAG, "Start service failed, error code = %s", esp_err_to_name(start_svc_ret));
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Create attribute table succeeded, but num_handle (%d) != ADA_IDX_NB (%d)", param->add_attr_tab.num_handle, ADA_IDX_NB);
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Create attribute table failed, error code %s", esp_err_to_name(param->add_attr_tab.status));
                }
                break;

            case ESP_GATTS_START_EVT:
                ESP_LOGI(TAG, "GATTS_START_EVT, status: %s, service_handle: %d",
                         esp_err_to_name(param->start.status), param->start.service_handle);
                if (param->start.status == ESP_GATT_OK)
                {
                    ESP_LOGI(TAG, "Service started successfully. Ready to advertise.");
                    // Consider initiating advertising from here if not already started,
                    // or post an event to app layer to indicate service is up.
                }
                else
                {
                    ESP_LOGE(TAG, "Service start failed, status %s", esp_err_to_name(param->start.status));
                }
                break;

            case ESP_GATTS_CONNECT_EVT:
                ESP_LOGI(TAG, "GATTS_CONNECT_EVT, conn_id: %d, if: %d, remote_bda: %02x:%02x:%02x:%02x:%02x:%02x",
                         param->connect.conn_id, gatts_if,
                         param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
                         param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5]);
                m_conn_id = param->connect.conn_id;
                m_gatts_if = gatts_if;
                memcpy(m_remote_bda, param->connect.remote_bda, ESP_BD_ADDR_LEN);
                m_is_app_gatt_connected = true;
                m_received_ssid = "";
                m_received_password = "";
                m_received_user_id = "";                                            // Clear received data
                m_ssid_received = m_password_received = m_user_id_received = false; // Reset flags

                if (m_is_advertising)
                {
                    esp_ble_gap_stop_advertising(); // Stop advertising as client is connected
                    ESP_LOGI(TAG, "Client connected, stopping advertising (if it was active).");
                }
                send_status_update_to_app(SETUP_STATUS_APP_CONNECTED, "Mobile app connected.");

                esp_event_post_to(m_app_event_loop_handle, ADA_APP_EVENT_BASE, APP_EVENT_BLE_DEV_CONNECTED, NULL, 0, portMAX_DELAY);

                esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);

                // Initiate pairing/bonding if desired (ESP32 acts as slave)
                // Example: esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
                // This will trigger security events (ESP_GAP_BLE_SEC_REQ_EVT, etc.)
                // For "Just Works" pairing, often no explicit call is needed if device is bondable and security is set.
                // Ensure GAP security parameters are set if bonding is required.
                // (e.g. esp_ble_gap_set_security_param)
                break;

            case ESP_GATTS_DISCONNECT_EVT:
                ESP_LOGI(TAG, "GATTS_DISCONNECT_EVT, conn_id: %d, if: %d, reason: 0x%x (%s)",
                         param->disconnect.conn_id, gatts_if, param->disconnect.reason, esp_err_to_name(param->disconnect.reason));
                m_is_app_gatt_connected = false;
                m_conn_id = 0xFFFF;
                memset(m_remote_bda, 0, ESP_BD_ADDR_LEN);

                esp_event_post_to(m_app_event_loop_handle, ADA_APP_EVENT_BASE, APP_EVENT_BLE_DEV_DISCONNECTED, NULL, 0, portMAX_DELAY);

                // If BLE is still initialized (i.e., setup not finalized and BLE disabled)
                // and not currently advertising (e.g. because a client was connected),
                // restart advertising to allow another setup attempt.
                if (m_is_ble_initialized && !m_is_advertising)
                {
                    ESP_LOGI(TAG, "Client disconnected, restarting advertising for setup.");
                    // Small delay before restarting advertising to allow stack to settle
                    // vTaskDelay(pdMS_TO_TICKS(100)); // Optional
                    esp_err_t start_ret = start_setup_mode();
                    if (start_ret != ESP_OK)
                    {
                        ESP_LOGE(TAG, "Failed to restart advertising after disconnect: %s", esp_err_to_name(start_ret));
                    }
                }
                break;

            case ESP_GATTS_WRITE_EVT:
                ESP_LOGI(TAG, "GATTS_WRITE_EVT, conn_id: %d, trans_id: %lu, handle: %d, len: %d, need_rsp: %d, is_prep: %d",
                         param->write.conn_id, param->write.trans_id, param->write.handle, param->write.len,
                         param->write.need_rsp, param->write.is_prep);

                if (param->write.is_prep)
                {
                    // TODO: Handle prepare write if characteristics can be > MTU - 3 bytes.
                    // For now, assume writes are not prepared (fit in single packet).
                    ESP_LOGW(TAG, "Prepare write received but not fully handled yet.");
                    if (param->write.need_rsp)
                    { // Always respond to prep writes
                        esp_gatt_rsp_t rsp;
                        memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
                        rsp.attr_value.handle = param->write.handle;
                        rsp.attr_value.len = param->write.len;
                        rsp.attr_value.offset = param->write.offset; // Important for prepare write
                        memcpy(rsp.attr_value.value, param->write.value, param->write.len);
                        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, &rsp);
                    }
                }
                else // Normal write or execute write (param->write.is_prep == 0)
                {
                    // Send response if client expects one for this normal write
                    if (param->write.need_rsp)
                    {
                        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
                    }

                    if (param->write.handle == m_gatts_handle_table[IDX_CHAR_VAL_SSID])
                    {
                        if (param->write.len <= MAX_SSID_LEN)
                        {
                            m_received_ssid.assign((char *)param->write.value, param->write.len);
                            m_ssid_received = true;
                            ESP_LOGI(TAG, "SSID received: %s", m_received_ssid.c_str());
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Received SSID too long (%d > %d)", param->write.len, MAX_SSID_LEN);
                        }
                    }
                    else if (param->write.handle == m_gatts_handle_table[IDX_CHAR_VAL_PASSWORD])
                    {
                        if (param->write.len <= MAX_PASSWORD_LEN)
                        {
                            m_received_password.assign((char *)param->write.value, param->write.len);
                            m_password_received = true;
                            ESP_LOGI(TAG, "Password received (length %d)", param->write.len);
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Received Password too long (%d > %d)", param->write.len, MAX_PASSWORD_LEN);
                        }
                    }
                    else if (param->write.handle == m_gatts_handle_table[IDX_CHAR_VAL_USER_ID])
                    {
                        if (param->write.len <= MAX_USER_ID_LEN)
                        {
                            m_received_user_id.assign((char *)param->write.value, param->write.len);
                            m_user_id_received = true;
                            ESP_LOGI(TAG, "User ID received: %s", m_received_user_id.c_str());
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Received User ID too long (%d > %d)", param->write.len, MAX_USER_ID_LEN);
                        }
                    }
                    else if (param->write.handle == m_gatts_handle_table[IDX_CHAR_CFG_STATUS])
                    {
                        if (param->write.len == 2)
                        { // CCCD value is 2 bytes
                            uint16_t ccc_val = param->write.value[0] | (param->write.value[1] << 8);
                            if (ccc_val & 0x0001) // Bit 0: Notifications enabled
                            {
                                ESP_LOGI(TAG, "Status Notifications ENABLED by client.");
                                s_char_status_ccc_val = ccc_val; // Store current CCCD value
                                send_status_update_to_app(SETUP_STATUS_APP_CONNECTED, "Notifications enabled by app.");
                            }
                            else if (ccc_val & 0x0002) // Bit 1: Indications enabled
                            {
                                ESP_LOGI(TAG, "Status Indications ENABLED by client.");
                                s_char_status_ccc_val = ccc_val;
                                send_status_update_to_app(SETUP_STATUS_APP_CONNECTED, "Indications enabled by app.");
                            }
                            else
                            {
                                ESP_LOGI(TAG, "Status Notifications/Indications DISABLED by client (CCC: 0x%04X).", ccc_val);
                                s_char_status_ccc_val = ccc_val;
                            }
                            // Update the attribute table value for CCCD if you want it to be persistent across reconnections (requires NVS storage for true persistence)
                            esp_ble_gatts_set_attr_value(m_gatts_handle_table[IDX_CHAR_CFG_STATUS], sizeof(s_char_status_ccc_val), (uint8_t *)&s_char_status_ccc_val);
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Invalid length for CCCD write: %d (expected 2)", param->write.len);
                        }
                    }
                    else if (param->write.handle == m_gatts_handle_table[IDX_CHAR_VAL_CONTROL])
                    {
                        if (param->write.len > 0)
                        { // Ensure there's at least one byte
                            ESP_LOGI(TAG, "Control point written, value[0]: 0x%02x", param->write.value[0]);
                            if (param->write.value[0] == 0x01) // Example: 0x01 = "Finalize Setup" command
                            {
                                ESP_LOGI(TAG, "Control: Client requested to finalize setup.");
                                esp_event_post_to(m_app_event_loop_handle, ADA_APP_EVENT_BASE, APP_EVENT_BLE_SETUP_FINISH_REQ, NULL, 0, portMAX_DELAY);
                            }
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Control point written with 0 length.");
                        }
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Write to unhandled characteristic handle: %d", param->write.handle);
                    }

                    if (m_ssid_received && m_password_received && m_user_id_received)
                    {
                        ESP_LOGI(TAG, "All credentials received via BLE.");
                        send_status_update_to_app(SETUP_STATUS_CREDS_RECEIVED, "Credentials processed by BLE manager.");
                        parse_and_post_credentials();
                        m_ssid_received = m_password_received = m_user_id_received = false; // Reset flags
                    }
                }
                break;

            case ESP_GATTS_EXEC_WRITE_EVT:
                ESP_LOGI(TAG, "GATTS_EXEC_WRITE_EVT, trans_id %lu, exec_write_flag: %d",
                         param->exec_write.trans_id, param->exec_write.exec_write_flag);
                // This event follows one or more ESP_GATTS_WRITE_EVT with is_prep=true.
                // If exec_write_flag is ESP_GATT_PREP_WRITE_EXEC, apply buffered writes.
                // If ESP_GATT_PREP_WRITE_CANCEL, discard them.
                // For now, we assume no long writes are used, so this might not be critical path.
                // Always send a response.
                esp_ble_gatts_send_response(gatts_if, param->exec_write.conn_id, param->exec_write.trans_id, ESP_GATT_OK, NULL);
                // TODO: If prepare writes were buffered, process or clear them here based on exec_write_flag
                break;

            case ESP_GATTS_READ_EVT:
                ESP_LOGI(TAG, "GATTS_READ_EVT, conn_id %d, trans_id %lu, handle %d, offset %d, need_rsp: %d",
                         param->read.conn_id, param->read.trans_id, param->read.handle, param->read.offset, param->read.need_rsp);

                esp_gatt_rsp_t rsp;
                memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
                rsp.attr_value.handle = param->read.handle; // Attribute handle being read
                // rsp.attr_value.offset = param->read.offset; // Offset is handled by stack if value is > MTU

                if (param->read.handle == m_gatts_handle_table[IDX_CHAR_VAL_STATUS])
                {
                    // The current status string is in `char_value_status` (updated by `send_status_update_to_app`)
                    // Ensure it's null-terminated if it's a C-string being read.
                    // `char_value_status` is already updated by `send_status_update_to_app` which calls `set_attr_value`
                    // The stack should provide the value. If manual response (RSP_BY_APP), copy it here.
                    // With AUTO_RSP, this might primarily be for logging or if a characteristic needs dynamic generation on read.

                    // Since we used AUTO_RSP for status value, the stack handles sending the value
                    // that was previously set by esp_ble_gatts_set_attr_value.
                    // However, if you need to dynamically generate it or check something:
                    size_t status_len = strlen((const char *)char_value_status);
                    rsp.attr_value.len = status_len > MAX_STATUS_MESSAGE_LEN ? MAX_STATUS_MESSAGE_LEN : status_len;
                    if (param->read.offset > rsp.attr_value.len)
                    {
                        rsp.attr_value.len = 0; // Offset beyond current length
                    }
                    else
                    {
                        rsp.attr_value.len -= param->read.offset;
                        memcpy(rsp.attr_value.value, char_value_status + param->read.offset, rsp.attr_value.len);
                    }
                    // For AUTO_RSP, this manual response might not be needed unless there's an error.
                    // If an error occurs or if you need to send a specific response:
                    // esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
                    ESP_LOGI(TAG, "Read for Status Char. Value provided by stack (AUTO_RSP). Current value: %s", (char *)char_value_status);
                }
                else if (param->read.handle == m_gatts_handle_table[IDX_CHAR_CFG_STATUS])
                {
                    // Client is reading the CCCD value
                    rsp.attr_value.len = sizeof(s_char_status_ccc_val);
                    memcpy(rsp.attr_value.value, &s_char_status_ccc_val, sizeof(s_char_status_ccc_val));
                    // For AUTO_RSP, stack handles this.
                    ESP_LOGI(TAG, "Read for Status CCCD. Current value: 0x%04X", s_char_status_ccc_val);
                }
                // Add other readable characteristics if any.
                // If all readable chars are AUTO_RSP and values set by set_attr_value, no explicit send_response is needed here.
                // Only send response if need_rsp is true and it's RSP_BY_APP or an error for AUTO_RSP.
                break;

            case ESP_GATTS_MTU_EVT:
                ESP_LOGI(TAG, "GATTS_MTU_EVT, conn_id: %d, MTU: %d", param->mtu.conn_id, param->mtu.mtu);
                // MTU has been exchanged. param->mtu.mtu is the new effective MTU.
                break;

            case ESP_GATTS_CONF_EVT:
                ESP_LOGI(TAG, "GATTS_CONF_EVT, status %s, handle %d, conn_id %d",
                         esp_err_to_name(param->conf.status), param->conf.handle, param->conf.conn_id);
                // This is a confirmation from the client that it received an Indication.
                // (Not for Notifications)
                if (param->conf.status != ESP_GATT_OK)
                {
                    ESP_LOGE(TAG, "Indication delivery failed, status: %s", esp_err_to_name(param->conf.status));
                }
                break;

            case ESP_GATTS_UNREG_EVT:
                ESP_LOGI(TAG, "GATTS_UNREG_EVT, app_id: %d", gatts_if); // gatts_if is the app_id here
                // m_gatts_if = ESP_GATT_IF_NONE; // Reset if this was our interface
                break;
            // case ESP_GATTS_DELETE_EVT:
            //     ESP_LOGI(TAG, "GATTS_DELETE_EVT, service_handle: %d, status: %s",
            //              param->del_svc.service_handle, esp_err_to_name(param->del_svc.status));
            //     break;
            case ESP_GATTS_STOP_EVT:
                ESP_LOGI(TAG, "GATTS_STOP_EVT, service_handle: %d, status: %s",
                         param->stop.service_handle, esp_err_to_name(param->stop.status));
                break;

            default:
                ESP_LOGD(TAG, "Unhandled GATTS Event: %d (%s)", event, esp_err_to_name(event));
                break;
            }
        }

        void AdaBluetoothManager::parse_and_post_credentials()
        {
            setup_event_data_t *creds_data = new setup_event_data_t();
            if (creds_data)
            {
                strncpy(creds_data->ssid, m_received_ssid.c_str(), sizeof(creds_data->ssid) - 1);
                strncpy(creds_data->password, m_received_password.c_str(), sizeof(creds_data->password) - 1);
                strncpy(creds_data->user_id, m_received_user_id.c_str(), sizeof(creds_data->user_id) - 1);

                creds_data->ssid[sizeof(creds_data->ssid) - 1] = '\0';
                creds_data->password[sizeof(creds_data->password) - 1] = '\0';
                creds_data->user_id[sizeof(creds_data->user_id) - 1] = '\0';

                esp_err_t post_ret = esp_event_post_to(m_app_event_loop_handle,
                                                       ADA_APP_EVENT_BASE,
                                                       APP_EVENT_BLE_SETUP_DATA_RECEIVED,
                                                       creds_data,
                                                       sizeof(setup_event_data_t),
                                                       portMAX_DELAY);

                delete creds_data;
                creds_data = nullptr;

                if (post_ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to post BLE setup data event: %s", esp_err_to_name(post_ret));
                    delete creds_data; // Clean up if post failed
                    send_status_update_to_app(SETUP_STATUS_ERROR, "Internal error posting credentials.");
                }
                else
                {
                    ESP_LOGI(TAG, "Posted credentials to app event loop.");
                }
            }
            else
            {
                ESP_LOGE(TAG, "Failed to allocate memory for setup_event_data_t");
                send_status_update_to_app(SETUP_STATUS_ERROR, "Internal error processing credentials (mem alloc).");
            }
        }

        std::string AdaBluetoothManager::create_status_json(setup_status_t status_code, const std::string &message)
        {
            cJSON *root = cJSON_CreateObject();
            if (!root)
            {
                ESP_LOGE(TAG, "Failed to create cJSON root object for status.");
                return "";
            }
            std::string status_str;

            switch (status_code)
            {
            case SETUP_STATUS_IDLE:
                status_str = "idle";
                break;
            case SETUP_STATUS_WAITING_FOR_APP_CONNECTION:
                status_str = "waiting_for_app";
                break;
            case SETUP_STATUS_APP_CONNECTED:
                status_str = "app_connected";
                break;
            case SETUP_STATUS_CREDS_RECEIVED:
                status_str = "creds_received";
                break;
            case SETUP_STATUS_WIFI_CONNECTING:
                status_str = "wifi_connecting";
                break;
            case SETUP_STATUS_COMPLETE:
                status_str = "setup_complete";
                break;
            case SETUP_STATUS_ERROR:
                status_str = "error";
                break;
            default:
                status_str = "unknown";
                break;
            }

            if (!cJSON_AddStringToObject(root, "status", status_str.c_str()))
            {
                ESP_LOGE(TAG, "Failed to add 'status' to cJSON object.");
                cJSON_Delete(root);
                return "";
            }
            if (!message.empty())
            {
                if (!cJSON_AddStringToObject(root, "message", message.c_str()))
                {
                    ESP_LOGE(TAG, "Failed to add 'message' to cJSON object.");
                    cJSON_Delete(root);
                    return "";
                }
            }

            char *json_string = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            if (json_string)
            {
                std::string result(json_string);
                cJSON_free(json_string); // Free memory allocated by cJSON_PrintUnformatted
                return result;
            }
            ESP_LOGE(TAG, "cJSON_PrintUnformatted failed.");
            return "";
        }

        esp_err_t AdaBluetoothManager::send_status_update_to_app(setup_status_t status_code, const std::string &message)
        {
            if (!m_is_ble_initialized)
            {
                ESP_LOGW(TAG, "Cannot update status, BLE not initialized.");
                return ESP_ERR_INVALID_STATE;
            }
            if (!m_is_app_gatt_connected || m_conn_id == 0xFFFF || m_gatts_if == ESP_GATT_IF_NONE)
            {
                // Log locally if app not connected, but don't try to send BLE notification
                ESP_LOGI(TAG, "App not connected. Status update (local log): %d - %s", status_code, message.c_str());
                // Still update char_value_status so a new connection gets the latest state on read/notify enable
                std::string temp_status_payload_str = create_status_json(status_code, message);
                if (!temp_status_payload_str.empty())
                {
                    strncpy((char *)char_value_status, temp_status_payload_str.c_str(), MAX_STATUS_MESSAGE_LEN - 1);
                    char_value_status[MAX_STATUS_MESSAGE_LEN - 1] = '\0'; // Ensure null termination
                    // If a handle is available (service created), update attribute value for future reads
                    if (m_gatts_handle_table[IDX_CHAR_VAL_STATUS] != 0)
                    {
                        esp_ble_gatts_set_attr_value(m_gatts_handle_table[IDX_CHAR_VAL_STATUS],
                                                     strlen((const char *)char_value_status),
                                                     (const uint8_t *)char_value_status);
                    }
                }
                return ESP_ERR_INVALID_STATE; // Or ESP_OK if local logging is sufficient
            }

            std::string status_payload_str = create_status_json(status_code, message);
            if (status_payload_str.empty())
            {
                ESP_LOGE(TAG, "Failed to create status JSON for app update.");
                return ESP_FAIL;
            }
            if (status_payload_str.length() >= MAX_STATUS_MESSAGE_LEN) // Use >= to account for null terminator if using strncpy later
            {
                ESP_LOGE(TAG, "Status payload too long: %d bytes (max %d). Truncating.", status_payload_str.length(), MAX_STATUS_MESSAGE_LEN);
                status_payload_str.resize(MAX_STATUS_MESSAGE_LEN - 1);
            }

            ESP_LOGI(TAG, "Updating app status (conn_id %d, handle %d): %s", m_conn_id, m_gatts_handle_table[IDX_CHAR_VAL_STATUS], status_payload_str.c_str());

            // Update the local characteristic value first, which is used for AUTO_RSP on reads
            // and as the source for notifications/indications.
            esp_err_t set_attr_ret = esp_ble_gatts_set_attr_value(m_gatts_handle_table[IDX_CHAR_VAL_STATUS],
                                                                  status_payload_str.length(),
                                                                  (const uint8_t *)status_payload_str.c_str());
            if (set_attr_ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to set attribute value for status: %s", esp_err_to_name(set_attr_ret));
                // return set_attr_ret; // Continue to attempt notification anyway? Or return error?
            }

            // Also update our static buffer `char_value_status` (which `gatt_db` might point to initially,
            // but `set_attr_value` updates the stack's internal copy). This ensures consistency if it's read elsewhere.
            memset(char_value_status, 0, MAX_STATUS_MESSAGE_LEN);
            strncpy((char *)char_value_status, status_payload_str.c_str(), MAX_STATUS_MESSAGE_LEN - 1);
            // char_value_status is already null-terminated by strncpy if src is shorter, or manually if truncated.

            // Check if client has enabled notifications or indications for the status characteristic
            // s_char_status_ccc_val should reflect the client's current CCCD setting.
            if (s_char_status_ccc_val & 0x0001) // Bit 0: Notifications enabled
            {
                ESP_LOGD(TAG, "Sending notification for status update.");
                return esp_ble_gatts_send_indicate(m_gatts_if, m_conn_id, m_gatts_handle_table[IDX_CHAR_VAL_STATUS],
                                                   status_payload_str.length(), (uint8_t *)status_payload_str.c_str(),
                                                   false); // false for Notification
            }
            else if (s_char_status_ccc_val & 0x0002) // Bit 1: Indications enabled
            {
                ESP_LOGD(TAG, "Sending indication for status update.");
                return esp_ble_gatts_send_indicate(m_gatts_if, m_conn_id, m_gatts_handle_table[IDX_CHAR_VAL_STATUS],
                                                   status_payload_str.length(), (uint8_t *)status_payload_str.c_str(),
                                                   true); // true for Indication (needs client ACK via ESP_GATTS_CONF_EVT)
            }
            else
            {
                ESP_LOGI(TAG, "Client has not enabled notifications/indications for status (CCC: 0x%04X). Status updated locally only.", s_char_status_ccc_val);
                return ESP_OK;
            }
        }

        esp_err_t AdaBluetoothManager::finalize_setup_and_disable_ble()
        {
            ESP_LOGI(TAG, "Finalizing setup: Disconnecting clients and disabling BLE.");

            // Stop advertising and disconnect any connected clients
            stop_setup_mode();
            // Note: stop_setup_mode initiates advertising stop and disconnection.
            // These are asynchronous. It might be cleaner to wait for disconnect/adv_stop events
            // before proceeding with deinitialization, or use a flag.
            // For simplicity here, we proceed after initiating the stop.

            // Deinitialize the BLE stack
            // Check if BLE was actually initialized before trying to deinit
            if (m_is_ble_initialized)
            {
                deinit_ble_stack();
                m_is_ble_initialized = false; // Mark as deinitialized
                ESP_LOGI(TAG, "BLE deinitialized and disabled.");
            }
            else
            {
                ESP_LOGW(TAG, "BLE was not initialized, skipping deinitialization.");
            }

            return ESP_OK;
        }

        void AdaBluetoothManager::deinit_ble_stack()
        {
            // Check if stack is already effectively down or never fully up.
            // m_is_ble_initialized flag is a better guard for this function.
            // if (!m_is_ble_initialized && m_gatts_if == ESP_GATT_IF_NONE) { ... }

            ESP_LOGI(TAG, "Deinitializing BLE stack (GATTS, GAP, Bluedroid, Controller).");
            esp_err_t ret;

            // Unregister GATTS application interface if it was registered
            if (m_gatts_if != ESP_GATT_IF_NONE)
            {
                ret = esp_ble_gatts_app_unregister(m_gatts_if);
                if (ret != ESP_OK)
                    ESP_LOGE(TAG, "GATTS app unregister failed: %s", esp_err_to_name(ret));
                m_gatts_if = ESP_GATT_IF_NONE; // Mark as unregistered
            }

            // Unregister callbacks
            // Note: It's good practice to unregister callbacks if they were registered.
            // However, if the app profile is unregistered, the stack might not call them anymore.
            // Check IDF documentation for specific behavior on unregistering callbacks after app_unregister.
            // For safety, unregister them.
            ret = esp_ble_gap_register_callback(NULL);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) // INVALID_STATE if not registered
                ESP_LOGE(TAG, "GAP unregister callback failed: %s", esp_err_to_name(ret));

            ret = esp_ble_gatts_register_callback(NULL);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
                ESP_LOGE(TAG, "GATTS unregister callback failed: %s", esp_err_to_name(ret));

            // Disable and deinitialize Bluedroid and BT Controller
            ret = esp_bluedroid_disable();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) // ESP_ERR_INVALID_STATE if already disabled
                ESP_LOGE(TAG, "Bluedroid disable failed: %s", esp_err_to_name(ret));

            ret = esp_bluedroid_deinit();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) // ESP_ERR_INVALID_STATE if already deinitialized
                ESP_LOGE(TAG, "Bluedroid deinit failed: %s", esp_err_to_name(ret));

            ret = esp_bt_controller_disable();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
                ESP_LOGE(TAG, "BT controller disable failed: %s", esp_err_to_name(ret));

            ret = esp_bt_controller_deinit();
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
                ESP_LOGE(TAG, "BT controller deinit failed: %s", esp_err_to_name(ret));

            esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
            // Or esp_bt_controller_mem_release(ESP_BT_MODE_BTDM); if it was full BTDM
        }

        bool AdaBluetoothManager::is_app_connected() const
        {
            return m_is_app_gatt_connected;
        }
    }
}
