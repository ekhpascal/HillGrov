idf_component_register(SRCS "app_main.c" "app_if_zone.c" "cmd_table_zone.c" "zone_ring.c" "zone_ring_cfg.c" "zone_ring_sync.c" INCLUDE_DIRS "."
    REQUIRES board cli cmd_task cmd_common app_common zone_cmds hg_store hg_model hg_cfg hg_blob notify rescue_handover
             app_update app_version esp_timer bootloader_support esp_driver_gpio esp_driver_uart ota_trial ring_link ring_proto)
