#ifndef ADA_BLUETOOTH_MANAGER
#define ADA_BLUETOOTH_MANAGER

#include "esp_err.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>
#include <string>
#include <vector>

#include "ada_global_events.hpp"
#include "ada_settings_manager.hpp"

namespace ada_assistant
{
  namespace bluetooth_manager
  {
    typedef enum
    {
      SETUP_STATUS_IDLE,
      SETUP_STATUS_WAITING_FOR_APP_CONNECTION,
      SETUP_STATUS_APP_CONNECTED,
      SETUP_STATUS_CREDS_RECEIVED,
      SETUP_STATUS_WIFI_CONNECTING,
      SETUP_STATUS_WIFI_CONNECTED,
      SETUP_STATUS_WIFI_FAILED,
      SETUP_STATUS_SERVER_PAIRING,
      SETUP_STATUS_SERVER_PAIRED,
      SETUP_STATUS_SERVER_PAIRING_FAILED,
      SETUP_STATUS_COMPLETE,
      SETUP_STATUS_ERROR
    } setup_status_t;

    typedef struct
    {
      setup_status_t status;
      std::string message;
    } setup_status_event_data_t;

    static const uint8_t ADA_VENDOR_BASE_UUID[ESP_UUID_LEN_128] = {
        0x1f, 0xcc, 0x0a, 0xa1, 0x24, 0x6e, 0x14, 0xbc,
        0x57, 0x49, 0x52, 0xc1, 0xf4, 0x27, 0x96, 0x4e};

#define ADA_SETUP_SERVICE_ALIAS 0xADA0
#define ADA_CHAR_SSID_ALIAS 0xADA1
#define ADA_CHAR_PASSWORD_ALIAS 0xADA2
#define ADA_CHAR_USER_ID_ALIAS 0xADA3
#define ADA_CHAR_STATUS_ALIAS 0xADA4
#define ADA_CHAR_CONTROL_ALIAS 0xADA5

#define MAX_SSID_LEN settings_manager::APP_MAX_SSID_BUFFER_LEN
#define MAX_PASSWORD_LEN settings_manager::APP_MAX_WIFI_PASSWORD_BUFFER_LEN
#define MAX_USER_ID_LEN settings_manager::APP_MAX_USER_ID_BUFFER_LEN

#define MAX_STATUS_MESSAGE_LEN 128

    typedef struct
    {
      char ssid[MAX_SSID_LEN];
      char password[MAX_PASSWORD_LEN];
      char user_id[MAX_USER_ID_LEN];
    } setup_event_data_t;

    enum
    {
      IDX_SVC,

      IDX_CHAR_SSID,
      IDX_CHAR_VAL_SSID,

      IDX_CHAR_PASSWORD,
      IDX_CHAR_VAL_PASSWORD,

      IDX_CHAR_USER_ID,
      IDX_CHAR_VAL_USER_ID,

      IDX_CHAR_STATUS,
      IDX_CHAR_VAL_STATUS,
      IDX_CHAR_CFG_STATUS, // Client Characteristic Configuration Descriptor for Status

      IDX_CHAR_CONTROL,
      IDX_CHAR_VAL_CONTROL,

      ADA_IDX_NB, // Total number of attributes
    };

    class AdaBluetoothManager
    {
    public:
      AdaBluetoothManager();
      ~AdaBluetoothManager();

      esp_err_t init(esp_event_loop_handle_t app_event_loop_handle);

      esp_err_t start_setup_mode();

      esp_err_t stop_setup_mode();

      esp_err_t send_status_update_to_app(setup_status_t status_code,
                                          const std::string &message = "");

      esp_err_t finalize_setup_and_disable_ble();

      bool is_app_connected() const;

    private:
      static void ble_gap_event_handler_bridge(esp_gap_ble_cb_event_t event,
                                               esp_ble_gap_cb_param_t *param);
      static void ble_gatts_event_handler_bridge(esp_gatts_cb_event_t event,
                                                 esp_gatt_if_t gatts_if,
                                                 esp_ble_gatts_cb_param_t *param);

      void ble_gap_event_handler(esp_gap_ble_cb_event_t event,
                                 esp_ble_gap_cb_param_t *param);
      void ble_gatts_event_handler(esp_gatts_cb_event_t event,
                                   esp_gatt_if_t gatts_if,
                                   esp_ble_gatts_cb_param_t *param);

      esp_err_t init_ble_stack();
      void deinit_ble_stack();

      std::string create_status_json(setup_status_t status_code,
                                     const std::string &message);
      void parse_and_post_credentials();

      esp_event_loop_handle_t m_app_event_loop_handle;

      bool m_is_ble_initialized;
      bool m_is_advertising;
      bool m_is_app_gatt_connected;
      uint16_t m_gatts_if;
      uint16_t m_conn_id;
      esp_bd_addr_t m_remote_bda;

      uint16_t m_gatts_handle_table[ADA_IDX_NB];

      std::string m_received_ssid;
      std::string m_received_password;
      std::string m_received_user_id;
      bool m_ssid_received;
      bool m_password_received;
      bool m_user_id_received;

      std::string m_adv_device_name;
      esp_ble_adv_data_t m_adv_data;
      esp_ble_adv_data_t m_scan_rsp_data;
      esp_ble_adv_params_t m_adv_params;

      SemaphoreHandle_t m_ble_op_mutex;
    };
  }
}

#endif /* ADA_BLUETOOTH_MANAGER */