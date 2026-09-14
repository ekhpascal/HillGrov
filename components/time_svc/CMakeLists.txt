idf_component_register(
    SRCS "time_svc.c"
    INCLUDE_DIRS "."
    REQUIRES lwip esp_netif time_core hg_mcfg hg_json app_common node_mgr
)
