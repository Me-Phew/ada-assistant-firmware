set(requires
    esp_timer
    ada_i2s_microphone_driver
    ada_i2s_speaker_driver
    ada_led_strip_driver
    )

idf_component_register(SRCS "src/ada_wake_word_detection_engine.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ${requires})
