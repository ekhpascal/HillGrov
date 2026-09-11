idf_component_register(SRCS "app_main.c" "app_if_master.c" "cmd_table_master.c" "net_ops_master.c" INCLUDE_DIRS "."
    REQUIRES board cli cmd_task cmd_common app_common notify app_update esp_timer nvs_flash ota_trial ring_link node_mgr master_cmds wifi_mgr fw_srv http_srv hg_mcfg time_svc alarm_mgr)

# hillgrow_embed_web() is NOT called here any more: the three gzipped web
# assets live in components/web_assets, whose CMakeLists calls it from its own
# directory scope. The function sets WHOLE_ARCHIVE on the component it embeds
# into, and on `main` that would defeat dead-code stripping for the whole
# application as it grows (Task 1 review).
