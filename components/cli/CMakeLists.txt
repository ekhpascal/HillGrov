idf_component_register(SRCS "cli.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_uart log freertos cli_line cmd_task notify esp_app_format board)
