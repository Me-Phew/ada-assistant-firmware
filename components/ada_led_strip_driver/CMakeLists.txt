set(requires
    esp_timer
    )

idf_component_register(SRCS "src/ada_led_strip_driver.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ${requires})
