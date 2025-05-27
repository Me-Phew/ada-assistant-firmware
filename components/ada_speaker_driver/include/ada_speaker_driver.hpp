#ifndef ADA_SPEAKER_DRIVER
#define ADA_SPEAKER_DRIVER

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "esp_peripherals.h"
#include "periph_touch.h"
#include "periph_adc_button.h"
#include "periph_button.h"
#include "board.h"
#include "esp_spiffs.h"
#include "http_stream.h"

#include "ada_global_events.hpp"

namespace ada_assistant
{
    namespace speaker_driver
    {
        struct ada_speaker_config_t
        {
            i2s_port_t i2s_port;

            uint32_t sample_rate;
            i2s_data_bit_width_t bits_per_sample;
        };

        class AdaSpeakerDriver
        {
        public:
            AdaSpeakerDriver(ada_speaker_config_t config);
            AdaSpeakerDriver();

            ~AdaSpeakerDriver();

            esp_err_t init(esp_event_loop_handle_t app_event_loop_handle, int initial_volume);

            esp_err_t deinit();

            bool is_running();
            bool is_stopped();

            esp_err_t play_mp3_file(std::string file_path);
            esp_err_t play_mp3_file_blocking(std::string file_path);

            esp_err_t play_http_stream(const char *url);
            esp_err_t play_http_stream_blocking(const char *url);

            esp_err_t pause();
            esp_err_t stop();
            esp_err_t resume();

            esp_err_t set_volume(int volume);
            int get_volume();

            esp_event_loop_handle_t app_event_loop_handle_;

            bool initialized;
            bool playing;
            int volume;

            audio_element_handle_t i2s_stream_writer_;
            audio_element_handle_t mp3_decoder_;
            audio_element_handle_t http_stream_reader_;
            audio_board_handle_t board_handle_;
            audio_pipeline_handle_t pipeline_;
            audio_event_iface_handle_t iface_handle_;

            static void audio_pipeline_task(void *pvParameters);
            TaskHandle_t pipeline_task_handle;

            esp_err_t _reset_and_clear_pipeline();

            i2s_port_t i2s_port;

            uint32_t sample_rate;
            i2s_data_bit_width_t bits_per_sample;

            std::string current_file_path_;
            uint8_t *file_buffer_;
            size_t file_size_;
            size_t file_position_;
            bool file_loaded_;

            esp_err_t load_file_to_memory(const std::string &file_path);
            void free_file_buffer();
            static audio_element_err_t file_read_callback(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx);
        };
    }
}

#endif /* ADA_SPEAKER_DRIVER */
