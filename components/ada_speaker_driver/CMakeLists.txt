idf_component_register(SRCS "src/ada_speaker_driver.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES
                        ada_application 
                        ada_settings_manager
                        esp_event 
                        audio_pipeline
                        audio_stream
                        audio_hal
                        esp_peripherals
                       )
