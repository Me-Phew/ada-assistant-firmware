#ifndef ADA_LED_STRIP_DRIVER
#define ADA_LED_STRIP_DRIVER

#include "esp_err.h"
#include "stdbool.h"

esp_err_t configure_led_strip(void);

esp_err_t start_rainbow_effect(void);

esp_err_t set_all_leds_to_color(
    uint8_t r,
    uint8_t g,
    uint8_t b);

esp_err_t clear_led_strip(void);

typedef void (*led_effect_finished_cb_t)(void);

esp_err_t ada_led_strip_start_sequential_fade_in_with_duration(
    uint16_t start_led,
    uint8_t target_r,
    uint8_t target_g,
    uint8_t target_b,
    uint32_t total_duration_ms,
    bool reverse);

esp_err_t ada_led_strip_start_sequential_fade_out_with_duration(
    uint16_t start_led,
    uint8_t start_r,
    uint8_t start_g,
    uint8_t start_b,
    uint32_t total_duration_ms,
    bool reverse);

esp_err_t ada_led_strip_start_color_breathing_with_duration(
    uint16_t start_led,
    uint8_t target_r,
    uint8_t target_g,
    uint8_t target_b,
    uint32_t total_duration_ms,
    uint8_t breath_cycles);

esp_err_t ada_led_strip_stop_effect(void);

esp_err_t set_all_leds_to_color(
    uint8_t r,
    uint8_t g,
    uint8_t b);

#endif /* ADA_LED_STRIP_DRIVER */
