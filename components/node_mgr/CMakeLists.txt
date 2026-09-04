idf_component_register(
    SRCS "node_mgr.c" "node_mgr_enrol.c" "node_mgr_fwd.c" "node_mgr_cfg.c" "node_mgr_cfgx.c"
         "fleet_seq.c" "node_mgr_fleet.c"
    INCLUDE_DIRS "."
    REQUIRES ring_proto ring_link node_store hg_blob hg_cfg notify freertos esp_timer fw_srv
)
