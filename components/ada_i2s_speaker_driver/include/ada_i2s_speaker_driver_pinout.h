#ifndef ADA_I2S_SPEAKER_DRIVER_PINOUT
#define ADA_I2S_SPEAKER_DRIVER_PINOUT

#include "driver/gpio.h"

#define GPIO_I2S_LRCK (CONFIG_ADA_I2S_SPEAKER_LRCK_GPIO)
#define GPIO_I2S_BCLK (CONFIG_ADA_I2S_SPEAKER_BCLK_GPIO)
#define GPIO_I2S_SDOUT (CONFIG_ADA_I2S_SPEAKER_SDOUT_GPIO)

#define I2S_CONFIG_DEFAULT(sample_rate, bits_per_chan, channel_fmt) {        \
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),                      \
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits_per_chan, channel_fmt), \
    .gpio_cfg = {                                                            \
        .mclk = I2S_GPIO_UNUSED,                                             \
        .bclk = GPIO_I2S_BCLK,                                               \
        .ws = GPIO_I2S_LRCK,                                                 \
        .dout = GPIO_I2S_SDOUT,                                              \
        .din = I2S_GPIO_UNUSED,                                              \
        .invert_flags = {                                                    \
            .mclk_inv = false,                                               \
            .bclk_inv = false,                                               \
            .ws_inv = false,                                                 \
        },                                                                   \
    },                                                                       \
}

#endif /* ADA_I2S_SPEAKER_DRIVER_PINOUT */
