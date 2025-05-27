#ifndef ADA_MICROPHONE_DRIVER
#define ADA_MICROPHONE_DRIVER

#include "esp_err.h"
#include <driver/i2s_std.h>
#include "esphome/components/i2s_audio/microphone/i2s_audio_microphone.h"

#include "ada_global_events.hpp"

namespace ada_assistant
{
    namespace microphone_driver
    {
#define MAX_EVENT_AUDIO_BYTES (20000 * sizeof(int32_t))

        typedef struct
        {
            int16_t *buffer_ptr;    // Pointer to the recording data in PSRAM
            size_t bytes_recorded;  // Actual number of bytes recorded into the buffer
            size_t buffer_capacity; // Original allocated size of the buffer in bytes
            int sample_rate;        // Sample rate of the recording
            int bits_per_sample;    // Bits per sample of the recording
        } event_audio_data_t;

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

            esp_err_t init(esp_event_loop_handle_t app_event_loop_handle);

            void setup();
            void start();
            void stop();

            void loop();

            bool is_running();
            bool is_stopped();

            bool is_recording();
            esp_err_t start_recording();
            esp_err_t stop_recording();

            size_t read(int16_t *buf, size_t len);

            TaskHandle_t recording_task_handle_;

        protected:
            const uint32_t total_recording_duration_s = 5;
            const uint32_t target_chunk_duration_ms = 500;
            const size_t mic_read_buffer_samples = 1024;
            static constexpr TickType_t read_loop_delay_ticks = pdMS_TO_TICKS(10);

            esp_event_loop_handle_t app_event_loop_handle_;

            gpio_num_t bck_pin;
            gpio_num_t sd_pin;
            gpio_num_t ws_pin;

            i2s_port_t i2s_port;

            int sample_rate;
            i2s_data_bit_width_t bits_per_sample;

            esphome::i2s_audio::I2SAudioMicrophone microphone_;

            static void recording_task(void *params);

            bool is_recording_;
        };
    }

}

#endif /* ADA_MICROPHONE_DRIVER */
