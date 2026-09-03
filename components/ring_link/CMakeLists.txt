idf_component_register(
    SRCS "ring_link.c"
    INCLUDE_DIRS "."
    REQUIRES ring_proto esp_driver_uart esp_driver_gpio freertos esp_timer board
)
