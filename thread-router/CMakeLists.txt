idf_component_register(SRCS "src/main.cpp"
                       INCLUDE_DIRS "src"
                       PRIV_REQUIRES nvs_flash openthread device_name power_management thread_comms esp_matter)
