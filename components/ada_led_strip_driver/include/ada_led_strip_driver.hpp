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

        class AdaLedStripDriver
        {
        public:
            AdaLedStripDriver();
            AdaLedStripDriver(ada_led_strip_driver_config_t config);

            ~AdaLedStripDriver();

            esp_err_t init(esp_event_loop_handle_t app_event_loop_handle, int initial_brightness);
            esp_err_t set_all_leds_to_color(uint8_t r, uint8_t g, uint8_t b);
            esp_err_t set_brightness(uint8_t brightness);

        private:
            esp_event_loop_handle_t app_event_loop_handle_;

            gpio_num_t gpio_num;

            uint16_t led_count_;

            led_strip_handle_t led_strip_handle_;

            uint8_t brightness_;

            bool backend_rmt_;
            bool backend_spi_;
        };
    } // namespace led_strip_driver

} // namespace ada_assistant

#endif /* ADA_LED_STRIP_DRIVER */
