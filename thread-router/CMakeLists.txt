idf_component_register(SRCS "src/main.cpp"
                             "src/bridge_nvs.cpp"
                             "src/proto/bridge_nvs.pb.c"
                       INCLUDE_DIRS "src"
                       PRIV_REQUIRES nvs_flash openthread device_name power_management thread_comms esp_matter nanopb)
