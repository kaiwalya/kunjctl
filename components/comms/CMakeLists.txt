# Only register this component if BLE is enabled
if(CONFIG_BT_ENABLED)
    idf_component_register(SRCS "comms.c" "proto/messages.pb.c"
                           INCLUDE_DIRS "include"
                           PRIV_REQUIRES bt nikas-belogolov__nanopb)
else()
    # Register empty component to avoid CMake errors
    idf_component_register()
endif()
