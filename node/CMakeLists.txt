idf_component_register(SRCS "src/main.c" "src/power_management.c"
                            "src/device_name.c"
                            "src/comms/comms.c"
                            "src/inputs/sensors.c" "src/outputs/status.c"
                            "src/proto/messages.pb.c" "src/state/state.c"
                    INCLUDE_DIRS "src" "src/comms" "src/inputs" "src/outputs" "src/proto" "src/state"
                    PRIV_REQUIRES esp_pm nvs_flash bt esp_driver_gpio led_strip)
