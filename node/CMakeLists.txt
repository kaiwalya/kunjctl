idf_component_register(SRCS "src/main.c"
                            "src/inputs/sensors.c" "src/outputs/status.c"
                            "src/state/state.c"
                       INCLUDE_DIRS "src" "src/inputs" "src/outputs" "src/state"
                       PRIV_REQUIRES nvs_flash esp_driver_gpio led_strip
                                     power_management device_name comms)
