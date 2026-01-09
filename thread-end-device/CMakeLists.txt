idf_component_register(SRCS "src/main.c"
                            "src/inputs/sensors.c"
                            "src/outputs/status.c"
                            "src/outputs/relay.c"
                       INCLUDE_DIRS "src" "src/inputs" "src/outputs"
                       PRIV_REQUIRES nvs_flash openthread device_name power_management
                                     esp_driver_gpio led_strip)
