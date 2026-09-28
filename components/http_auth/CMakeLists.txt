# The web-session store (the ONE wa_state_t: live login sessions + the sha/rand
# hooks web_auth needs), split out of http_srv so components/panel_svc can call
# http_auth_hash_password()/http_auth_sessions_drop() while http_srv itself
# requires panel_svc -- no dependency cycle. Master-only sources, include-only
# on zone/rescue, exactly the http_srv pattern (components/http_srv/CMakeLists.txt).
# REQUIRES is unconditional on purpose: IDF evaluates it in an early pass where
# CMAKE_PROJECT_NAME is not yet set (components/wifi_mgr/CMakeLists.txt:14-19).
set(HTTP_AUTH_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    set(HTTP_AUTH_SRCS "http_auth.c")
endif()

idf_component_register(
    SRCS ${HTTP_AUTH_SRCS}
    INCLUDE_DIRS "."
    REQUIRES web_auth hg_mcfg
    PRIV_REQUIRES freertos esp_hw_support nvs_flash mbedtls app_common time_svc
)
