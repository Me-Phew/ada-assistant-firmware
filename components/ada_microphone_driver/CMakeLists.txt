idf_component_register(SRCS "src/ada_microphone_driver.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ada_application driver micro_wake_word
                       )
