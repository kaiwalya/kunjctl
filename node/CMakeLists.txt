idf_component_register(SRCS "main.c" "power_management.c" "ble_adv.c" "status.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_pm nvs_flash bt led_strip)
