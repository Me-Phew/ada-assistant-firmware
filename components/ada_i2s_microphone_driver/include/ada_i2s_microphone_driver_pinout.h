#ifndef ADA_I2S_MICROPHONE_DRIVER_PINOUT
#define ADA_I2S_MICROPHONE_DRIVER_PINOUT

#include "driver/gpio.h"

#define GPIO_I2S_LRCK (CONFIG_ADA_I2S_MICROPHONE_LRCK_GPIO)
#define GPIO_I2S_SCLK (CONFIG_ADA_I2S_MICROPHONE_SCLK_GPIO)
#define GPIO_I2S_SDIN (CONFIG_ADA_I2S_MICROPHONE_SDIN_GPIO)

#define I2S_CONFIG_DEFAULT(sample_rate, channel_fmt, bits_per_chan) {            \
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),                          \
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_chan, channel_fmt), \
    .gpio_cfg = {                                                                \
        .mclk = GPIO_NUM_NC,                                                     \
        .bclk = GPIO_I2S_SCLK,                                                   \
        .ws = GPIO_I2S_LRCK,                                                     \
        .dout = GPIO_NUM_NC,                                                     \
        .din = GPIO_I2S_SDIN,                                                    \
        .invert_flags = {                                                        \
            .mclk_inv = false,                                                   \
            .bclk_inv = false,                                                   \
            .ws_inv = false,                                                     \
        },                                                                       \
    },                                                                           \
}

#endif /* ADA_I2S_MICROPHONE_DRIVER_PINOUT */
