idf_component_register(SRCS "app_main.c" "app_if_zone.c" "cmd_table_zone.c" INCLUDE_DIRS "."
    REQUIRES board cli cmd_task cmd_common zone_cmds hg_store hg_model hg_cfg notify rescue_handover app_update esp_timer bootloader_support esp_driver_gpio)
