idf_component_register(SRCS "src/ada_application.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES 
                           ada_settings_manager 
                           ada_speaker_driver
                           ada_bluetooth_manager
                           ada_wifi_manager
                           ada_cloud_services
                           ada_microphone_driver 
                           ada_wake_word_detection_engine
                           esp_driver_gpio 
                           nvs_flash 
                           esp_event
                           spiffs
                      )
