idf_component_register(SRCS "src/ada_cloud_services.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ada_application esp_event esp_http_client esp-tls
                       )
