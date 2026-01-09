idf_component_register(SRCS "src/main.c"
                       INCLUDE_DIRS "src"
                       PRIV_REQUIRES esp_coex esp_event nvs_flash openthread)
