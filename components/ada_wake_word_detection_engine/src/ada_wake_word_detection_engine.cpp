#include "esp_log.h"
#include "esp_event.h"

#include "ada_wake_word_detection_engine.hpp"
#include "hey_ada.h"

#include "ada_global_events.h"

namespace ada_assistant
{
    namespace wake_word_detection_engine
    {
        static const char *TAG = "ADA_WWD_ENGINE";

        static const std::string WAKE_WORD_NAME = "Hey Ada";

        AdaWakeWordDetectionEngine::AdaWakeWordDetectionEngine(ada_wake_word_detection_engine_config_t config) : wakeWord_(), wwd_task_handle_(NULL), microphone_(config.microphone), app_event_loop_handle_(nullptr) {}

        esp_err_t AdaWakeWordDetectionEngine::init(esp_event_loop_handle_t app_event_loop_handle)
        {
            this->app_event_loop_handle_ = app_event_loop_handle;

            this->wakeWord_.set_microphone(this->microphone_);
            this->wakeWord_.add_wake_word_model(hey_ada_tflite, 0.97f, 5, WAKE_WORD_NAME, hey_ada_tflite_len);
            this->wakeWord_.set_features_step_size(10);
            this->wakeWord_.add_detection_callback([this](std::string detected_wake_word)
                                                   { this->onWakeWordDetected(detected_wake_word); });

            this->wakeWord_.setup();

            ESP_LOGI(TAG, "AdaWakeWordDetectionEngine initialized.");

            return ESP_OK;
        }

        esp_err_t AdaWakeWordDetectionEngine::start()
        {
            if (this->wwd_task_handle_ != NULL)
            {
                ESP_LOGE(TAG, "Wake word processing task already running");
                return ESP_FAIL;
            }

            BaseType_t task_created = xTaskCreatePinnedToCore(
                wakeWordDetectionTask,
                "ada_wwd_task",
                4096,
                this,
                2,
                &this->wwd_task_handle_,
                1);

            if (task_created != pdPASS || this->wwd_task_handle_ == NULL)
            {
                ESP_LOGE(TAG, "Failed to create wake word processing task!");
                this->wwd_task_handle_ = NULL;

                return ESP_FAIL;
            }

            ESP_LOGI(TAG, "Wake word processing task created successfully.");

            return ESP_OK;
        }

        esp_err_t AdaWakeWordDetectionEngine::onWakeWordDetectionTaskStopped()
        {
            ESP_LOGI(TAG, "Wake word detection task stopped.");
            esp_err_t ret = esp_event_post_to(this->app_event_loop_handle_,
                                              ADA_APP_EVENT_BASE,
                                              ADA_WAKE_WORD_DETECTION_STOPPED,
                                              NULL,
                                              0,
                                              portMAX_DELAY);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to post ADA_WAKE_WORD_DETECTION_STOPPED: %s", esp_err_to_name(ret));
            }

            return ret;
        }

        esp_err_t AdaWakeWordDetectionEngine::stop()
        {
            if (this->wwd_task_handle_ != NULL)
            {
                this->wakeWord_.stop();

                vTaskDelay(pdMS_TO_TICKS(100));

                if (this->wwd_task_handle_ != NULL)
                {
                    vTaskDelete(this->wwd_task_handle_);
                    this->wwd_task_handle_ = NULL;
                }

                ESP_LOGI(TAG, "Wake word processing task deleted successfully.");

                return onWakeWordDetectionTaskStopped();
            }

            return ESP_FAIL;
        }

        void AdaWakeWordDetectionEngine::onWakeWordDetected(std::string detected_wake_word)
        {
            ESP_LOGI(TAG, "Wake word detected!");

            esp_err_t ret = esp_event_post_to(this->app_event_loop_handle_,
                                              ADA_APP_EVENT_BASE,
                                              ADA_WAKE_WORD_DETECTED,
                                              detected_wake_word.c_str(),
                                              detected_wake_word.length() + 1,
                                              portMAX_DELAY);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to post APP_EVENT_BUTTON_PRESSED: %s", esp_err_to_name(ret));
            }
        }

        void AdaWakeWordDetectionEngine::wakeWordDetectionTask(void *params)
        {
            AdaWakeWordDetectionEngine *self = static_cast<AdaWakeWordDetectionEngine *>(params);

            ESP_LOGI(TAG, "Wake word processing task started.");

            self->wakeWord_.start();

            while (true)
            {
                if (!self || !self->wakeWord_.is_running())
                {
                    ESP_LOGI(TAG, "Wake word processing task stopped.");
                    break;
                }

                self->wakeWord_.loop();

                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (self->wakeWord_.is_running())
            {
                self->wakeWord_.stop();
            }

            self->onWakeWordDetectionTaskStopped();

            self->wwd_task_handle_ = NULL;
            vTaskDelete(NULL);
        }

        AdaWakeWordDetectionEngine::~AdaWakeWordDetectionEngine()
        {
            this->stop();
        }
    }
}