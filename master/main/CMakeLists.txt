idf_component_register(SRCS "app_main.c" "app_if_master.c" "cmd_table_master.c" INCLUDE_DIRS "."
    REQUIRES board cli cmd_task cmd_common app_common notify app_update esp_timer nvs_flash ota_trial ring_link node_mgr master_cmds)
