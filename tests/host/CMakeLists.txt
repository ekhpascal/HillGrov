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
  ${COMP}/time_core ${COMP}/wifi_mgr
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
        ${COMP}/cmd_common/cmd_common.c ${COMP}/ota_trial/trial_cmds.c fakes/fake_node_ops.c fakes/fake_net_ops.c)
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
