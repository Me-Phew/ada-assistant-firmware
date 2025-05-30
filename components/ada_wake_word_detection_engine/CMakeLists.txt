idf_component_register(SRCS "src/ada_wake_word_detection_engine.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event ada_application ada_microphone_driver micro_wake_word
                       )
