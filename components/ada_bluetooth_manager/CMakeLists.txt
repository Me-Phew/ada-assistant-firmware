idf_component_register(SRCS "src/ada_bluetooth_manager.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ada_application ada_settings_manager esp_event bt json
                       )
