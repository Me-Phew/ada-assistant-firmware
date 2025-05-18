#ifndef ADA_WAKE_WORD_DETECTION_ENGINE
#define ADA_WAKE_WORD_DETECTION_ENGINE

#include "esp_event.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"

#include "ada_microphone_driver.hpp"

namespace ada_assistant
{
    namespace wake_word_detection_engine
    {
        struct ada_wake_word_detection_engine_config_t
        {
            ada_assistant::microphone_driver::AdaMicrophoneDriver *microphone;
        };

        class AdaWakeWordDetectionEngine
        {
        public:
            AdaWakeWordDetectionEngine(ada_wake_word_detection_engine_config_t config);

            ~AdaWakeWordDetectionEngine();

            esp_err_t init(esp_event_loop_handle_t app_event_loop_handle);

            esp_err_t start();
            esp_err_t stop();

            esphome::micro_wake_word::MicroWakeWord wakeWord_;
            TaskHandle_t wwd_task_handle_;

        protected:
            ada_assistant::microphone_driver::AdaMicrophoneDriver *microphone_;

        private:
            esp_event_loop_handle_t app_event_loop_handle_;

            static void wakeWordDetectionTask(void *params);

            void onWakeWordDetected(std::string detected_wake_word);

            esp_err_t onWakeWordDetectionTaskStopped();
        };
    }
}

#endif /* ADA_WAKE_WORD_DETECTION_ENGINE */
