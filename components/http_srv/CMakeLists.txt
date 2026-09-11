# Master-only component: the web UI, the CLI-over-HTTP endpoint and the fleet
# firmware pull all live on the master. The sources are guarded the same way
# hg_mcfg guards mcfg_store.c (IDF configures components after the top-level
# project() call, so CMAKE_PROJECT_NAME already names the app being built) --
# for the zone and rescue builds this stays an include-only component, so
# neither image compiles an HTTP server it can never reach.
#
# http_routes.c is also compiled straight from source by the host suite
# (tests/host/CMakeLists.txt, test_http_routes) -- it is pure C with no IDF
# dependency precisely so the route table and its auth bits are testable.
set(HTTP_SRV_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    list(APPEND HTTP_SRV_SRCS "http_routes.c" "http_srv.c" "http_auth.c" "http_login.c" "http_cmd.c" "http_static.c")
endif()

idf_component_register(
    SRCS ${HTTP_SRV_SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp_http_server cmd_core cmd_task web_auth hg_mcfg web_assets fw_srv
    PRIV_REQUIRES app_common time_svc hg_blob nvs_flash mbedtls esp_hw_support espressif__cjson
)
