#ifndef ADA_APPLICATION
#define ADA_APPLICATION

#include "esp_err.h"
#include "esp_event.h"

#include "ada_microphone_driver.hpp"
#include "ada_wake_word_detection_engine.hpp"

namespace ada_assistant
{
    class AdaApplication
    {
    public:
        AdaApplication();

        esp_err_t init();

        ~AdaApplication();

    private:
        esp_event_loop_handle_t app_event_loop_handle_;

        microphone_driver::AdaMicrophoneDriver microphone_;
        wake_word_detection_engine::AdaWakeWordDetectionEngine wake_word_engine_;

        void app_event_handler(esp_event_base_t event_base, int32_t event_id, void *event_data);

        static void app_event_handler_bridge(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data);
    };
}

#endif /* ADA_APPLICATION */
