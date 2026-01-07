idf_component_register(SRCS "src/main.c" "src/power_management.c" "src/ble_adv.c"
                            "src/device_name.c"
                            "src/inputs/sensors.c" "src/outputs/status.c"
                            "src/proto/messages.pb.c"
                    INCLUDE_DIRS "src" "src/inputs" "src/outputs" "src/proto"
                    PRIV_REQUIRES esp_pm nvs_flash bt esp_driver_gpio led_strip)
