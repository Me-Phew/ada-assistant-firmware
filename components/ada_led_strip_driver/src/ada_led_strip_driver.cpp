#include "esp_err.h"
#include "esp_log.h"

#include "ada_led_strip_driver.hpp"

namespace ada_assistant
{
    namespace led_strip_driver
    {
        static const char *TAG = "ADA_LED_STRIP_DRIVER";

        AdaLedStripDriver::AdaLedStripDriver()
            : app_event_loop_handle_(nullptr), gpio_num((gpio_num_t)CONFIG_ADA_LED_STRIP_GPIO_NUM), led_count_(CONFIG_ADA_LED_STRIP_STRIP_LED_COUNT), led_strip_handle_(nullptr), brightness_(128),
              backend_rmt_(false), backend_spi_(false)
        {
            ESP_LOGI(TAG, "AdaLedStripDriver constructor");

#if CONFIG_ADA_LED_STRIP_BACKEND_RMT
            backend_rmt_ = true;
#elif CONFIG_ADA_LED_STRIP_BACKEND_SPI
            backend_spi_ = true;
#else
            ESP_LOGE(TAG, "No valid backend selected");
#endif
        }

        AdaLedStripDriver::AdaLedStripDriver(ada_led_strip_driver_config_t config)
            : app_event_loop_handle_(nullptr), gpio_num(config.gpio_num), led_count_(config.led_count), led_strip_handle_(nullptr), brightness_(config.brightness),
              backend_rmt_(config.backend_rmt_), backend_spi_(config.backend_spi_)
        {
            ESP_LOGI(TAG, "AdaLedStripDriver constructor with config");
        }

        AdaLedStripDriver::~AdaLedStripDriver()
        {
            ESP_LOGI(TAG, "AdaLedStripDriver destructor");
        }

        esp_err_t AdaLedStripDriver::init(esp_event_loop_handle_t app_event_loop_handle, int initial_brightness)
        {
            ESP_LOGI(TAG, "Initializing LED strip driver on GPIO %d", gpio_num);
            ESP_LOGI(TAG, "LED count: %d", led_count_);
            ESP_LOGI(TAG, "Backend RMT: %s", backend_rmt_ ? "true" : "false");
            ESP_LOGI(TAG, "Backend SPI: %s", backend_spi_ ? "true" : "false");
            ESP_LOGI(TAG, "Initial brightness: %d", initial_brightness);

            brightness_ = initial_brightness;

            led_strip_config_t strip_config = {
                .strip_gpio_num = gpio_num,
                .max_leds = led_count_,
                // .led_pixel_format = LED_PIXEL_FORMAT_GRB,
                .led_model = LED_MODEL_WS2812,
                // .flags = {
                //     .invert_out = false,
                // },
            };

            if (backend_rmt_)
            {
                led_strip_rmt_config_t rmt_config = {
                    .resolution_hz = 10 * 1000 * 1000, // 10MHz
                    .flags = {
                        .with_dma = false,
                    },
                };
                ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_handle_));
            }
            else if (backend_spi_)
            {
                led_strip_spi_config_t spi_config = {
                    .spi_bus = SPI2_HOST,
                    .flags = {
                        .with_dma = true,
                    },
                };
                ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip_handle_));
            }
            else
            {
                ESP_LOGE(TAG, "No valid backend selected");
                return ESP_ERR_INVALID_STATE;
            }

            esp_err_t res = led_strip_clear(led_strip_handle_);

            return res;
        }

        esp_err_t AdaLedStripDriver::set_all_leds_to_color(uint8_t red, uint8_t green, uint8_t blue)
        {
            ESP_LOGI(TAG, "Setting LED color to R:%d G:%d B:%d", red, green, blue);

            esp_err_t res = ESP_FAIL;

            for (int i = 0; i < led_count_; i++)
            {
                led_strip_set_pixel(led_strip_handle_, i, red, green, blue);
            }

            res = led_strip_refresh(led_strip_handle_);
            if (res != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to refresh LED strip: %s", esp_err_to_name(res));
            }

            return res;
        }

        esp_err_t AdaLedStripDriver::set_brightness(uint8_t brightness)
        {
            ESP_LOGI(TAG, "Setting LED brightness to %d", brightness);

            brightness_ = brightness;

            // Set the brightness of the LED strip here

            return ESP_OK;
        }
    } // namespace led_strip_driver
} // namespace ada_assistant
