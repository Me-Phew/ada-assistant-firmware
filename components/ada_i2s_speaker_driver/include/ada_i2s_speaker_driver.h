#ifndef ADA_I2S_SPEAKER_DRIVER
#define ADA_I2S_SPEAKER_DRIVER

#include "esp_err.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

/**
 * @brief Initialize the I2S speaker driver
 *
 * Configures and installs the I2S driver for speaker output (Master TX mode).
 *
 * @param config Pointer to the configuration structure. Must not be NULL.
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Parameter error (e.g., NULL config, invalid pins, invalid port)
 *      - ESP_ERR_NO_MEM: Memory allocation failed
 *      - ESP_ERR_INVALID_STATE: Driver already installed or not in suitable state
 *      - ESP_FAIL: Failed to install driver or set pins
 */
esp_err_t ada_i2s_speaker_init(void);

/**
 * @brief Deinitialize the I2S speaker driver
 *
 * Uninstalls the I2S driver for the specified port.
 *
 * @param i2s_port The I2S port number to deinitialize.
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid I2S port number
 *      - ESP_FAIL: Failed to uninstall driver
 */
esp_err_t ada_i2s_speaker_deinit(void);

esp_err_t enable_i2s_channel(void);

esp_err_t disable_i2s_channel(void);

/**
 * @brief Write audio data to the I2S speaker
 *
 * Sends audio data to the I2S DMA buffer for playback.
 * This function will block until all data is written or the timeout expires.
 *
 * @param src Pointer to the audio data buffer.
 *            The data format must match the configured bits_per_sample and channel_format.
 *            E.g., for 16-bit stereo, data should be interleaved [Left_Sample1, Right_Sample1, Left_Sample2, Right_Sample2, ...].
 *            E.g., for 16-bit mono (using I2S_CHANNEL_FMT_ONLY_LEFT), data should be [Sample1, Sample2, Sample3, ...].
 * @param size Size of the audio data buffer in bytes.
 * @param[out] bytes_written Pointer to store the number of bytes actually written.
 * @param ticks_to_wait Ticks to wait for space in the DMA buffer. Use portMAX_DELAY to wait indefinitely.
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Parameter error (e.g., invalid port, NULL src, size is 0)
 *      - ESP_FAIL: Failed to write data (driver not installed or other error)
 *      - ESP_ERR_TIMEOUT: Timeout waiting for DMA buffer space
 */
esp_err_t ada_i2s_speaker_write(const void *src, size_t size, size_t *bytes_written, TickType_t ticks_to_wait);

typedef void (*audio_playback_finished_cb_t)(void);

esp_err_t ada_i2s_start_file_playback(const char *filename);

esp_err_t ada_i2s_stop_playback(void);

#endif /* ADA_I2S_SPEAKER_DRIVER */
