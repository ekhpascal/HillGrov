# components/panel_svc: the shared service layer BOTH faces call -- the web's
# HTTP handlers and the P4 panel (spec Decision 4: "Both faces call the layer
# beneath HTTP"). Master-only sources, include-only on zone/rescue: the
# http_srv pattern. REQUIRES stays unconditional (evaluated in IDF's early
# requirements pass, where CMAKE_PROJECT_NAME is not yet set) and names only
# components present in every app build.
set(PSVC_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    list(APPEND PSVC_SRCS "psvc_rc.c" "psvc_mcfg.c" "psvc_net.c")
endif()

idf_component_register(
    SRCS ${PSVC_SRCS}
    INCLUDE_DIRS "."
    REQUIRES hg_cfg hg_mcfg master_cmds wifi_mgr
    PRIV_REQUIRES mcfg_ops time_core hg_json time_svc http_auth node_mgr
)
