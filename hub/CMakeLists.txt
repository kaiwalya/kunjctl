idf_component_register(SRCS "src/main.c"
                       INCLUDE_DIRS "src"
                       PRIV_REQUIRES nvs_flash
                                     power_management device_name comms)
