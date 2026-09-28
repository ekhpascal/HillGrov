# components/panel_svc: the shared service layer BOTH faces call -- the web's
# HTTP handlers and the P4 panel (spec Decision 4: "Both faces call the layer
# beneath HTTP"). Master-only sources, include-only on zone/rescue: the
# http_srv pattern. REQUIRES stays unconditional (evaluated in IDF's early
# requirements pass, where CMAKE_PROJECT_NAME is not yet set) and names only
# components present in every app build.
set(PSVC_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    list(APPEND PSVC_SRCS "psvc_rc.c" "psvc_mcfg.c" "psvc_net.c" "psvc_zcfg.c"
                          "psvc_state.c" "psvc_state_conv.c" "psvc_fw.c" "psvc_fleet.c"
                          "psvc_fw_env.c" "fw_sink_master.c" "fw_sink_zone.c")
endif()

idf_component_register(
    SRCS ${PSVC_SRCS}
    INCLUDE_DIRS "."
    REQUIRES hg_cfg hg_mcfg master_cmds wifi_mgr ring_proto state_snap
    PRIV_REQUIRES mcfg_ops time_core hg_json time_svc http_auth node_mgr alarm_mgr app_common
                  esp_timer esp_app_format heap
                  app_update esp_partition fw_srv hg_blob hg_image ota_trial esp_system freertos
)
