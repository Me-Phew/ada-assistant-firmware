#ifndef ADA_I2S_MICROPHONE_DRIVER
#define ADA_I2S_MICROPHONE_DRIVER

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"

/**
 * @brief Init I2S microphone driver
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t i2s_microphone_init(void);

esp_err_t i2s_microphone_deinit(void);

/**
 * @brief Get the record pcm data.
 *
 * @param buffer The buffer where the data is stored.
 * @param buffer_len The buffer length.
 * @return
 *    - ESP_OK                  Success
 *    - Others                  Fail
 */
esp_err_t i2s_microphone_get_feed_data(int16_t *buffer, int buffer_len);

/**
 * @brief Get the record channel number.
 *
 * @return The record channel number.
 */
int i2s_microphone_get_feed_channel(void);

/**
 * @brief Get the input format of the board for ESP-SR.
 *
 * @return The input format of the board, like "MMR"
 */
char *i2s_microphone_get_input_format(void);

#endif /* ADA_I2S_MICROPHONE_DRIVER */
