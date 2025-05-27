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

        esp_err_t AdaLedStripDriver::clear_led_strip()
        {
            ESP_LOGI(TAG, "Clearing LED strip");

            esp_err_t res = led_strip_clear(led_strip_handle_);
            if (res != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to clear LED strip: %s", esp_err_to_name(res));
            }

            return res;
        }

        void AdaLedStripDriver::flashing_effect_task(void *params)
        {
            AdaLedStripDriver *self = static_cast<AdaLedStripDriver *>(params);
            ESP_LOGI(TAG, "Effect task started");

            while (self->is_effect_running_)
            {
                for (int i = 0; i < self->led_count_; i++)
                {
                    led_strip_set_pixel(self->led_strip_handle_, i, self->current_effect_config_.r * self->current_effect_config_.brightness,
                                        self->current_effect_config_.g * self->current_effect_config_.brightness,
                                        self->current_effect_config_.b * self->current_effect_config_.brightness);
                }
                led_strip_refresh(self->led_strip_handle_);
                vTaskDelay(pdMS_TO_TICKS(self->current_effect_config_.duration_ms));

                for (int i = 0; i < self->led_count_; i++)
                {
                    led_strip_clear(self->led_strip_handle_);
                }

                led_strip_refresh(self->led_strip_handle_);
                vTaskDelay(pdMS_TO_TICKS(self->current_effect_config_.duration_ms));
            }

            ESP_LOGI(TAG, "Effect task finished");
            vTaskDelete(NULL);
        }

        esp_err_t AdaLedStripDriver::stop_current_effect()
        {
            ESP_LOGI(TAG, "Stopping current effect");

            if (!is_effect_running_)
            {
                ESP_LOGW(TAG, "No effect is currently running");
                return ESP_ERR_INVALID_STATE;
            }

            is_effect_running_ = false;

            size_t wait_time_ms = 0;
            size_t max_wait_time_ms = 1000;

            while (effect_task_handle_ != NULL && wait_time_ms > max_wait_time_ms)
            {
                ESP_LOGI(TAG, "Waiting for effect task to finish...");
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_time_ms += 10;
            }

            if (effect_task_handle_ != NULL)
            {

                vTaskDelete(effect_task_handle_);
                effect_task_handle_ = NULL;
            }

            ESP_LOGI(TAG, "Effect task deleted successfully");
            return ESP_OK;
        }

        bool AdaLedStripDriver::is_effect_running() const
        {
            return is_effect_running_;
        }

        esp_err_t AdaLedStripDriver::start_flashing_effect(ada_led_strip_effect_config_t effect_config)
        {
            ESP_LOGI(TAG, "Starting flashing effect with color R:%d G:%d B:%d for %d ms", effect_config.r, effect_config.g, effect_config.b, effect_config.duration_ms);

            if (effect_config.brightness == -1)
            {
                effect_config.brightness = brightness_;
            }

            effect_config.brightness = effect_config.brightness / 100;

            current_effect_config_ = effect_config;
            ESP_LOGI(TAG, "Effect config set: R:%d G:%d B:%d Duration:%d ms Brightness:%d", effect_config.r, effect_config.g, effect_config.b, effect_config.duration_ms, effect_config.brightness);

            is_effect_running_ = true;

            BaseType_t task_created = xTaskCreatePinnedToCore(
                flashing_effect_task,
                "ada_led_strip_effect_task",
                4096,
                this,
                10,
                &effect_task_handle_,
                1);

            if (task_created != pdPASS || effect_task_handle_ == NULL)
            {
                ESP_LOGE(TAG, "Failed to create effect task!");
                effect_task_handle_ = NULL;
                is_effect_running_ = false;
                return ESP_FAIL;
            }

            return ESP_OK;
        }

        esp_err_t AdaLedStripDriver::set_all_leds_to_color(uint8_t red, uint8_t green, uint8_t blue, int brightness)
        {
            ESP_LOGI(TAG, "Setting LED color to R:%d G:%d B:%d", red, green, blue);

            if (brightness == -1)
            {
                brightness = brightness_;
            }

            brightness = brightness / 100;

            esp_err_t res = ESP_FAIL;

            for (int i = 0; i < led_count_; i++)
            {
                led_strip_set_pixel(led_strip_handle_, i, red * brightness, green * brightness, blue * brightness);
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
