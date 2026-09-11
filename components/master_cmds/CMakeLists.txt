# hg_mcfg + wifi_mgr are here for master_cmds.h's own includes (hg_mcfg_t and
# wifi_status_t in net_ops_t), i.e. PUBLIC include dirs -- not for any symbol:
# master_cmds.c calls nothing in either, it only ever goes through net_ops_t.
# wifi_mgr registers as a header-only component on the zone build for exactly
# this reason (components/wifi_mgr/CMakeLists.txt).
idf_component_register(SRCS "master_cmds.c" INCLUDE_DIRS "."
    REQUIRES cmd_core ring_proto hg_mcfg wifi_mgr)
