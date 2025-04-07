#include "string.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "ada_i2s_microphone_driver.h"
#include "ada_i2s_microphone_driver_pinout.h"

static const char *TAG = "ADA_I2S_MIC_DRIVER";

#define I2S_NUM I2S_NUM_AUTO

#define SAMPLE_RATE 16000
#define CHANNEL_FORMAT I2S_SLOT_MODE_MONO
#define BITS_PER_CHAN 32

static i2s_chan_handle_t tx_handle = NULL; // I2S rx channel handler

esp_err_t i2s_microphone_init(void)
{
    esp_err_t ret_val = ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);

    ret_val |= i2s_new_channel(&chan_cfg, NULL, &tx_handle);
    i2s_std_config_t std_cfg = I2S_CONFIG_DEFAULT(SAMPLE_RATE, CHANNEL_FORMAT, BITS_PER_CHAN);
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    // std_cfg.clk_cfg.mclk_multiple = EXAMPLE_MCLK_MULTIPLE;   //The default is I2S_MCLK_MULTIPLE_256. If not using 24-bit data width, 256 should be enough
    ret_val |= i2s_channel_init_std_mode(tx_handle, &std_cfg);
    ret_val |= i2s_channel_enable(tx_handle);

    return ret_val;
}

esp_err_t i2s_microphone_deinit(void)
{
    esp_err_t ret_val = ESP_OK;

    ret_val |= i2s_channel_disable(tx_handle);
    ret_val |= i2s_del_channel(tx_handle);
    tx_handle = NULL;

    return ret_val;
}

esp_err_t i2s_microphone_get_feed_data(int16_t *buffer, int buffer_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_read;
    int audio_chunksize = buffer_len / (sizeof(int32_t));
    ret = i2s_channel_read(tx_handle, buffer, buffer_len, &bytes_read, portMAX_DELAY);

    int32_t *tmp_buff = (int32_t *)buffer;
    for (int i = 0; i < audio_chunksize; i++)
    {
        tmp_buff[i] = tmp_buff[i] >> 14; // Bits 32:8 are valid, bits 8:0 are all zeros (lower 8 bits). AFE input is 16-bit audio data. Using bits 29:13 to amplify the audio signal.
    }

    return ret;
}

int i2s_microphone_get_feed_channel(void)
{
    return I2S_NUM;
}

char *i2s_microphone_get_input_format(void)
{
    return "MN";
}
