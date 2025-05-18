idf_component_register(SRCS "src/ada_application.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio nvs_flash esp_event ada_microphone_driver ada_wake_word_detection_engine
                       )
