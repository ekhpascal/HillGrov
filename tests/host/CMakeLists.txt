cmake_minimum_required(VERSION 3.16)
project(hillgrow_host_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(unity
  URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.0.tar.gz)
FetchContent_MakeAvailable(unity)
FetchContent_Declare(cjson
  URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
  SOURCE_SUBDIR does-not-exist)   # cJSON ships its own (very old) CMakeLists.txt; we only
                                  # want the source tree, built ourselves below, so this
                                  # keeps FetchContent from add_subdirectory()-ing it.
FetchContent_MakeAvailable(cjson)
add_library(cjson_host STATIC ${cjson_SOURCE_DIR}/cJSON.c)
target_include_directories(cjson_host PUBLIC ${cjson_SOURCE_DIR})
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
set(COMP ${CMAKE_CURRENT_SOURCE_DIR}/../../components)
include_directories(
  ${COMP}/app_version ${COMP}/hg_blob ${COMP}/hg_cfg ${COMP}/hg_model ${COMP}/hg_mcfg
  ${COMP}/cli_line ${COMP}/notify ${COMP}/cmd_core ${COMP}/cmd_common
  ${COMP}/zone_cmds ${COMP}/rescue_handover ${COMP}/ring_proto ${COMP}/node_store ${COMP}/ota_trial
  ${COMP}/master_cmds ${COMP}/node_mgr ${COMP}/ring_link ${COMP}/web_auth ${COMP}/hg_json ${COMP}/alarm_mgr
  ${COMP}/time_core ${COMP}/wifi_mgr ${COMP}/state_snap ${COMP}/http_srv ${COMP}/mcfg_ops ${COMP}/cp_ota
  ${COMP}/panel_svc ${COMP}/time_svc ${COMP}/panel_ui ${COMP}/hg_image
  ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
enable_testing()
function(hg_test NAME)
  add_executable(${NAME} ${NAME}.c fakes/fake_clock.c ${ARGN})
  target_link_libraries(${NAME} unity)
  target_compile_definitions(${NAME} PRIVATE HOST_TEST=1)
  add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

hg_test(test_app_version ${COMP}/app_version/app_version.c)
hg_test(test_hg_blob ${COMP}/hg_blob/hg_blob.c)
hg_test(test_hg_cfg_types ${COMP}/hg_cfg/hg_cfg_defaults.c)
set(HG_CFG_SRC ${COMP}/hg_cfg/hg_cfg_defaults.c ${COMP}/hg_cfg/hg_cfg_fields.c ${COMP}/hg_cfg/hg_cfg_validate.c)
hg_test(test_hg_cfg_fields   ${HG_CFG_SRC})
hg_test(test_hg_cfg_validate ${HG_CFG_SRC})
hg_test(test_hg_model ${COMP}/hg_model/hg_model.c ${HG_CFG_SRC} ${COMP}/hg_blob/hg_blob.c)
hg_test(test_cli_line ${COMP}/cli_line/cli_line.c)
hg_test(test_notify ${COMP}/notify/notify.c)
set(CMD_CORE_SRC ${COMP}/cmd_core/cmd_tok.c ${COMP}/cmd_core/cmd_resp.c ${COMP}/cmd_core/cmd_dispatch.c ${COMP}/cmd_core/cmd_help.c)
hg_test(test_cmd_core ${CMD_CORE_SRC})
hg_test(test_cmd_help ${CMD_CORE_SRC})
hg_test(test_zone_cmds ${COMP}/zone_cmds/zone_cmds.c ${CMD_CORE_SRC} ${HG_CFG_SRC} ${COMP}/hg_model/hg_model.c
        ${COMP}/cmd_common/cmd_common.c ${COMP}/notify/notify.c ${COMP}/ota_trial/trial_cmds.c ${COMP}/hg_blob/hg_blob.c)
hg_test(test_cmd_common ${COMP}/cmd_common/cmd_common.c ${CMD_CORE_SRC} ${COMP}/notify/notify.c fakes/fake_app_if.c)
hg_test(test_handover ${COMP}/rescue_handover/rescue_handover.c ${COMP}/hg_blob/hg_blob.c)
hg_test(test_ring_frame ${COMP}/ring_proto/ring_crc.c ${COMP}/ring_proto/ring_cobs.c ${COMP}/ring_proto/ring_frame.c)
hg_test(test_ring_route ${COMP}/ring_proto/ring_route.c ${COMP}/ring_proto/ring_dup.c)
hg_test(test_ring_trk ${COMP}/ring_proto/ring_trk.c ${COMP}/ring_proto/ring_frame.c ${COMP}/ring_proto/ring_crc.c ${COMP}/ring_proto/ring_cobs.c)
hg_test(test_ring_tlm ${COMP}/ring_proto/ring_tlm_hb.c ${COMP}/ring_proto/ring_tlm.c)
hg_test(test_ring_cfgx ${COMP}/ring_proto/ring_cfgx.c)
hg_test(test_ring_health ${COMP}/ring_proto/ring_health.c ${COMP}/ring_proto/ring_tlm_hb.c ${COMP}/ring_proto/ring_tlm.c)
hg_test(test_node_store ${COMP}/node_store/ztab.c ${COMP}/hg_blob/hg_blob.c)
hg_test(test_ota_trial ${COMP}/ota_trial/trial_eval.c ${COMP}/ota_trial/trial_cmds.c ${CMD_CORE_SRC})
hg_test(test_master_cmds ${COMP}/master_cmds/master_cmds.c ${CMD_CORE_SRC} ${COMP}/notify/notify.c
        ${COMP}/cmd_common/cmd_common.c ${COMP}/ota_trial/trial_cmds.c fakes/fake_node_ops.c fakes/fake_net_ops.c
        ${COMP}/panel_ui/pnl_cli.c fakes/nvs_fake/fake_nvs.c)
# CLEAR PANEL CONFIRM (pnl_cli.c) calls nvs_*: a fake nvs.h for this test only, never on the shared include path
target_include_directories(test_master_cmds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fakes/nvs_fake)
hg_test(test_fleet_seq ${COMP}/node_mgr/fleet_seq.c)
hg_test(test_hg_mcfg ${COMP}/hg_mcfg/hg_mcfg.c ${HG_CFG_SRC} ${COMP}/hg_blob/hg_blob.c)
hg_test(test_web_auth ${COMP}/web_auth/web_auth.c ${COMP}/hg_mcfg/hg_mcfg.c ${HG_CFG_SRC} ${COMP}/hg_blob/hg_blob.c fakes/fake_sha256.c)
hg_test(test_hg_json ${COMP}/hg_json/hg_json_schema.c ${COMP}/hg_json/hg_json_cfg.c ${COMP}/hg_json/hg_json_mcfg.c
        ${COMP}/hg_mcfg/hg_mcfg.c ${HG_CFG_SRC} ${COMP}/hg_blob/hg_blob.c)
target_link_libraries(test_hg_json cjson_host)
hg_test(test_alarm_mgr ${COMP}/alarm_mgr/alarm_mgr.c ${COMP}/alarm_mgr/alarm_mgr_json.c ${COMP}/notify/notify.c)
target_link_libraries(test_alarm_mgr cjson_host)
hg_test(test_time_core ${COMP}/time_core/time_core.c)
hg_test(test_node_mgr_cfg ${COMP}/node_mgr/node_mgr_cfg.c ${COMP}/node_mgr/node_mgr_cfgx.c
        ${COMP}/node_mgr/node_mgr_cfg_api.c ${COMP}/node_mgr/node_mgr_cfg_latch.c ${COMP}/ring_proto/ring_cfgx.c ${COMP}/hg_blob/hg_blob.c
        ${HG_CFG_SRC} ${COMP}/notify/notify.c fakes/fake_nmgr.c)
hg_test(test_state_snap ${COMP}/state_snap/state_snap.c)
target_link_libraries(test_state_snap cjson_host)
hg_test(test_http_routes ${COMP}/http_srv/http_routes.c)
hg_test(test_http_body_sizes)
target_link_libraries(test_http_body_sizes cjson_host)
hg_test(test_partitions_p4)
target_compile_definitions(test_partitions_p4 PRIVATE
    HG_PARTITIONS_P4_CSV="${CMAKE_CURRENT_LIST_DIR}/../../master/partitions_p4.csv")

# mcfg_ops.c is real production code, compiled straight from source like
# http_routes.c above -- only what is underneath it is faked: the FreeRTOS
# mutex (fakes/freertos/*.h, header-only) and mcfg_store.c's NVS-backed
# mcfg_get()/mcfg_commit() (fakes/fake_mcfg_store.c, a RAM-only stand-in that
# still runs the REAL hg_mcfg_validate()). Same shape as test_node_mgr_cfg.c
# linking node_mgr's real decision logic against fake_nmgr.c instead of the
# real, hardware-touching node_mgr.c.
hg_test(test_mcfg_ops ${COMP}/mcfg_ops/mcfg_ops.c ${COMP}/hg_mcfg/hg_mcfg.c ${HG_CFG_SRC}
        ${COMP}/hg_blob/hg_blob.c fakes/fake_mcfg_store.c)

# cp_ota.c compiles straight from source, same as http_routes.c and
# mcfg_ops.c above: only cp_ota_needed() and cp_ota_parse_header() are pure
# and host-tested here. cp_ota_sync() (the RPC push) is guarded out under
# HOST_TEST -- see components/cp_ota/cp_ota.c -- so this test link never
# needs esp_partition, esp_task_wdt or esp_hosted at all. hg_blob.c is
# linked in for hg_crc32 -- the test builds body_crc values the same way
# production code does (fix round 1, Task 6 concern 1: cp_fw carries the
# same HGFW header zone_fw does, and cp_ota_parse_header() checks it).
hg_test(test_cp_ota ${COMP}/cp_ota/cp_ota.c ${COMP}/hg_blob/hg_blob.c)

# panel_svc (panel plan Task 3): the shared service layer both faces call.
# psvc_rc.c is pure; psvc_mcfg.c runs over the REAL mcfg_ops.c, hg_mcfg
# validation, hg_json merge and time_core tz_check -- only the NVS store
# (fake_mcfg_store.c) and the two radio/time apply hooks (fake_apply.c) are faked.
hg_test(test_psvc_rc ${COMP}/panel_svc/psvc_rc.c ${COMP}/panel_svc/psvc_fleet.c)
hg_test(test_psvc_mcfg ${COMP}/panel_svc/psvc_mcfg.c ${COMP}/panel_svc/psvc_rc.c ${COMP}/mcfg_ops/mcfg_ops.c
        ${COMP}/hg_mcfg/hg_mcfg.c ${HG_CFG_SRC} ${COMP}/hg_blob/hg_blob.c ${COMP}/hg_json/hg_json_mcfg.c
        ${COMP}/time_core/time_core.c fakes/fake_mcfg_store.c fakes/fake_apply.c)
target_link_libraries(test_psvc_mcfg cjson_host)

# psvc_zcfg.c over the REAL hg_cfg validation and hg_json merge; only node_mgr's
# three §4.4 primitives are scripted (fakes/fake_nmgr_cfg_api.c).
hg_test(test_psvc_zcfg ${COMP}/panel_svc/psvc_zcfg.c ${COMP}/panel_svc/psvc_rc.c ${HG_CFG_SRC}
        ${COMP}/hg_json/hg_json_cfg.c ${COMP}/hg_mcfg/hg_mcfg.c ${COMP}/hg_blob/hg_blob.c fakes/fake_nmgr_cfg_api.c)
target_link_libraries(test_psvc_zcfg cjson_host)

# panel_ui's PURE helpers only (panel plan Global Constraints, "Pure files"):
# they include no lvgl.h / bsp / esp_* / freertos header, so they compile here.
hg_test(test_pnl_touch ${COMP}/panel_ui/pnl_touch.c)

# psvc_state_conv.c (pure: the snap_master_t conversion and h_state's two text
# parsers) against the REAL state_snap writer; psvc_fw.c's progress
# publication. psvc_state.c itself is IDF glue (node_mgr, wifi_mgr, heap).
hg_test(test_psvc_state ${COMP}/panel_svc/psvc_state_conv.c ${COMP}/panel_svc/psvc_fw.c ${COMP}/state_snap/state_snap.c ${COMP}/hg_image/hg_image.c)
target_link_libraries(test_psvc_state cjson_host)

# Task 10: pure presentation helpers (time, formatting, home-screen maths).
# No lvgl.h / bsp / esp_* / freertos header, same "pure files" rule as pnl_touch.c.
hg_test(test_pnl_time ${COMP}/panel_ui/pnl_time.c)
hg_test(test_pnl_fmt  ${COMP}/panel_ui/pnl_fmt.c ${COMP}/panel_ui/pnl_time.c)
hg_test(test_pnl_home ${COMP}/panel_ui/pnl_home.c ${COMP}/panel_ui/pnl_time.c ${COMP}/hg_cfg/hg_cfg_defaults.c)

# panel_ui's config-widget generator (Task 17): pure, linked against the real field tables.
hg_test(test_pcfg_gen ${COMP}/panel_ui/pcfg_gen.c ${COMP}/panel_ui/pcfg_pres.c ${HG_CFG_SRC}
        ${COMP}/hg_mcfg/hg_mcfg.c ${COMP}/hg_blob/hg_blob.c)

# panel_ui's config edit model and refusal texts (Task 18): pure.
hg_test(test_pcfg_edit ${COMP}/panel_ui/pcfg_edit.c ${HG_CFG_SRC} ${COMP}/hg_mcfg/hg_mcfg.c ${COMP}/hg_blob/hg_blob.c)
hg_test(test_pnl_msg ${COMP}/panel_ui/pnl_msg.c ${COMP}/panel_svc/psvc_rc.c)

# panel_ui's keyboard filters and MAC parser (Task 19): pure.
hg_test(test_pnl_input ${COMP}/panel_ui/pnl_input.c)

# panel_ui zone console model (Task 22): pure.
hg_test(test_pnl_console ${COMP}/panel_ui/pnl_console.c)

# panel_ui panel-local preferences and their persisted form (Task 26): pure.
hg_test(test_pnl_prefs ${COMP}/panel_ui/pnl_prefs.c ${COMP}/hg_blob/hg_blob.c)

# panel_ui night dimming decision (Task 27): pure.
hg_test(test_pnl_dim ${COMP}/panel_ui/pnl_dim.c ${COMP}/panel_ui/pnl_home.c ${COMP}/panel_ui/pnl_time.c ${HG_CFG_SRC})

# hg_image shared app-image identity (Task 28; recovery design 4.2 / 5.7): pure. The three CMakeLists paths let the test
# pin each HG_PROJ_* constant against the project() line that defines it.
hg_test(test_hg_image ${COMP}/hg_image/hg_image.c)
target_compile_definitions(test_hg_image PRIVATE
    HG_MASTER_CMAKELISTS="${CMAKE_CURRENT_LIST_DIR}/../../master/CMakeLists.txt"
    HG_ZONE_CMAKELISTS="${CMAKE_CURRENT_LIST_DIR}/../../zone/CMakeLists.txt"
    HG_COPROC_CMAKELISTS="${CMAKE_CURRENT_LIST_DIR}/../../coproc/CMakeLists.txt")

# panel_svc's firmware-install core (Task 30): pure, behind a fake environment, sink and reader. The web upload and the
# panel's microSD install both run it.
hg_test(test_psvc_fw ${COMP}/panel_svc/psvc_fw.c ${COMP}/panel_svc/psvc_rc.c ${COMP}/hg_image/hg_image.c)

# panel_ui's microSD firmware picker (Task 31): pure -- classifies and orders the .bin files on the card from their
# 112-byte headers through the shared hg_image identity. The mount itself (pnl_sd.c) is glue, proven only on the bench.
hg_test(test_pnl_sd_pick ${COMP}/panel_ui/pnl_sd_pick.c ${COMP}/hg_image/hg_image.c)
