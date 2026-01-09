# Thread comms component - only works with OpenThread enabled
idf_component_register(
    SRCS "thread_comms.c" "proto/messages.pb.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "proto"
    PRIV_REQUIRES openthread nikas-belogolov__nanopb
)
