#include "esp_err.h"
#include "esp_log.h"
#include <driver/i2s_std.h>

#include "ada_microphone_driver.hpp"

namespace ada_assistant
{
    namespace microphone_driver
    {

        static const char *TAG = "ADA_MIC_DRIVER";

        AdaMicrophoneDriver::AdaMicrophoneDriver() : bck_pin((gpio_num_t)CONFIG_ADA_MICROPHONE_DRIVER_SCK_GPIO),
                                                     sd_pin((gpio_num_t)CONFIG_ADA_MICROPHONE_DRIVER_SD_GPIO),
                                                     ws_pin((gpio_num_t)CONFIG_ADA_MICROPHONE_DRIVER_WS_GPIO),
                                                     i2s_port((i2s_port_t)CONFIG_ADA_MICROPHONE_DRIVER_I2S_PORT),
                                                     sample_rate(CONFIG_ADA_MICROPHONE_DRIVER_SAMPLE_RATE),
                                                     bits_per_sample((i2s_data_bit_width_t)CONFIG_ADA_MICROPHONE_DRIVER_BITS_PER_SAMPLE),
                                                     microphone_()
        {
        }

        AdaMicrophoneDriver::AdaMicrophoneDriver(ada_microphone_config_t config) : bck_pin(config.bck_pin),
                                                                                   sd_pin(config.sd_pin),
                                                                                   ws_pin(config.ws_pin),
                                                                                   i2s_port(config.i2s_port),
                                                                                   sample_rate(config.sample_rate),
                                                                                   bits_per_sample(config.bits_per_sample),
                                                                                   microphone_()
        {
        }

        esp_err_t AdaMicrophoneDriver::init()
        {
            esp_err_t ret_val = ESP_OK;

            ESP_LOGI(TAG, "Initializing microphone driver...");
            ESP_LOGI(TAG, "BCK Pin: %d", this->bck_pin);
            ESP_LOGI(TAG, "WS Pin: %d", this->ws_pin);
            ESP_LOGI(TAG, "SD Pin: %d", this->sd_pin);

            ESP_LOGI(TAG, "I2S Port: %d", this->i2s_port);

            ESP_LOGI(TAG, "Sample Rate: %d", this->sample_rate);
            ESP_LOGI(TAG, "Bits per Sample: %d", this->bits_per_sample);

            this->microphone_.set_bclk_pin(this->bck_pin);
            this->microphone_.set_lrclk_pin(this->ws_pin);
            this->microphone_.set_din_pin(this->sd_pin);

            this->microphone_.set_channel(I2S_CHANNEL_DEFAULT_CONFIG(this->i2s_port, I2S_ROLE_MASTER));

            this->microphone_.set_sample_rate(this->sample_rate);
            this->microphone_.set_bits_per_sample(this->bits_per_sample);

            return ret_val;
        }

        void AdaMicrophoneDriver::setup()
        {
            this->microphone_.setup();
        }

        void AdaMicrophoneDriver::start()
        {
            this->microphone_.start();
        }

        void AdaMicrophoneDriver::stop()
        {
            this->microphone_.stop();
        }

        void AdaMicrophoneDriver::loop()
        {
            this->microphone_.loop();
        }

        size_t AdaMicrophoneDriver::read(int16_t *buf, size_t len)
        {
            return this->microphone_.read(buf, len);
        }

        bool AdaMicrophoneDriver::is_running()
        {
            return this->microphone_.is_running();
        }

        bool AdaMicrophoneDriver::is_stopped()
        {
            return this->microphone_.is_stopped();
        }

        esp_err_t AdaMicrophoneDriver::deinit()
        {
            esp_err_t ret_val = ESP_OK;

            this->microphone_.stop();

            return ret_val;
        }

        AdaMicrophoneDriver::~AdaMicrophoneDriver()
        {
            this->deinit();
        }

    }
}
