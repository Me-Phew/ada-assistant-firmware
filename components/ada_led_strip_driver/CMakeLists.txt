idf_component_register(SRCS "src/ada_led_strip_driver.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ada_application ada_settings_manager led_strip esp_timer
                       )
