#ifndef ADA_MICROPHONE_DRIVER
#define ADA_MICROPHONE_DRIVER

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include <driver/i2s_std.h>
#include "esphome/components/i2s_audio/microphone/i2s_audio_microphone.h"

namespace ada_assistant
{
    namespace microphone_driver
    {
        struct ada_microphone_config_t
        {
            gpio_num_t bck_pin;
            gpio_num_t sd_pin;
            gpio_num_t ws_pin;

            i2s_port_t i2s_port;

            int sample_rate;
            i2s_data_bit_width_t bits_per_sample;
        };

        class AdaMicrophoneDriver
        {
        public:
            AdaMicrophoneDriver();
            AdaMicrophoneDriver(ada_microphone_config_t);

            ~AdaMicrophoneDriver();

            esp_err_t init();

            esp_err_t deinit();

            void setup();
            void start();
            void stop();

            void loop();

            bool is_running();
            bool is_stopped();

            size_t read(int16_t *buf, size_t len);

        protected:
            gpio_num_t bck_pin;
            gpio_num_t sd_pin;
            gpio_num_t ws_pin;

            i2s_port_t i2s_port;

            int sample_rate;
            i2s_data_bit_width_t bits_per_sample;

            esphome::i2s_audio::I2SAudioMicrophone microphone_;
        };
    }

}

#endif /* ADA_MICROPHONE_DRIVER */
