#ifndef ADA_LED_STRIP_DRIVER
#define ADA_LED_STRIP_DRIVER

#include "driver/gpio.h"
#include "led_strip.h"

#include "ada_global_events.hpp"

namespace ada_assistant
{
    namespace led_strip_driver
    {
        struct ada_led_strip_driver_config_t
        {
            gpio_num_t gpio_num;
            uint16_t led_count;
            bool backend_rmt_;
            bool backend_spi_;

            uint8_t brightness;
        };

        struct ada_led_strip_effect_config_t
        {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            int duration_ms;
            int brightness = -1;
        };

        class AdaLedStripDriver
        {
        public:
            AdaLedStripDriver();
            AdaLedStripDriver(ada_led_strip_driver_config_t config);

            ~AdaLedStripDriver();

            esp_err_t init(esp_event_loop_handle_t app_event_loop_handle, int initial_brightness);
            esp_err_t set_all_leds_to_color(uint8_t r, uint8_t g, uint8_t b, int brightness = -1);
            esp_err_t set_brightness(uint8_t brightness);

            esp_err_t start_flashing_effect(ada_led_strip_effect_config_t effect_config);

            esp_err_t stop_current_effect();

            bool is_effect_running() const;

            esp_err_t clear_led_strip();

            TaskHandle_t effect_task_handle_;

        private:
            esp_event_loop_handle_t app_event_loop_handle_;

            gpio_num_t gpio_num;

            uint16_t led_count_;

            led_strip_handle_t led_strip_handle_;

            uint8_t brightness_;

            bool backend_rmt_;
            bool backend_spi_;

            static void flashing_effect_task(void *params);

            bool is_effect_running_;

            ada_led_strip_effect_config_t current_effect_config_;
        };
    } // namespace led_strip_driver

} // namespace ada_assistant

#endif /* ADA_LED_STRIP_DRIVER */
