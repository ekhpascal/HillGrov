idf_component_register(SRCS "app_main.c" "app_if_master.c" "cmd_table_master.c" "net_ops_master.c" INCLUDE_DIRS "."
    REQUIRES board cli cmd_task cmd_common app_common notify app_update esp_timer nvs_flash ota_trial ring_link node_mgr master_cmds wifi_mgr fw_srv hg_mcfg time_svc web_auth mbedtls esp_hw_support)

# hillgrow_embed_web must be called from the same directory scope that owns the
# component's library target (this one), not from the top-level master/CMakeLists.txt --
# a generated source's producing custom command is invisible across CMake directory
# scopes to the Ninja generator, so calling it from the parent left the .gz->.S asm
# source with no real build rule (CMake silently substitutes a no-op stub).
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/hillgrow_web.cmake)
hillgrow_embed_web(main)
