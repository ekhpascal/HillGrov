# wifi_mgr is master-only: it calls mcfg_get() (hg_mcfg's mcfg_store.c, itself
# master-only for the same reason) and pulls in espressif__mdns, which only
# master/main/idf_component.yml declares. IDF puts every component in
# components/ into BOTH app builds, so on the zone build this compiles no
# sources at all and contributes only its include dir -- which the zone build
# genuinely needs, because master_cmds.h (compiled there too) embeds
# wifi_status_t from wifi_mgr.h.
#
# Same conditional-source mechanism as components/hg_mcfg/CMakeLists.txt, and
# the same caveat: IDF components are configured after the top-level project()
# call, so CMAKE_PROJECT_NAME names the app being built (master/CMakeLists.txt
# is project(hillgrow_master), zone/CMakeLists.txt project(hillgrow_zone)).
#
# The REQUIRES list, unlike SRCS, must NOT be put inside that condition: IDF
# reads REQUIRES during its early-expansion pass over this file, where
# CMAKE_PROJECT_NAME is not yet set, so a conditional REQUIRES silently
# evaluates to empty and the sources then compile with no esp_wifi/esp_netif
# include dirs at all (observed: "fatal error: esp_wifi.h: No such file or
# directory" with every REQUIRES-provided path missing from the command line).
# Every component listed here exists in both builds. espressif__mdns does not,
# so it is attached after registration with idf_component_optional_requires --
# the idiom espressif__mdns itself uses for esp_wifi -- and only on the build
# that actually has sources to compile (PRIVATE linkage is meaningless on the
# INTERFACE library an empty-SRCS registration produces).
set(WIFI_MGR_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    set(WIFI_MGR_SRCS "wifi_mgr.c" "wifi_mgr_sta.c" "wifi_mgr_scan.c")
endif()

idf_component_register(
    SRCS ${WIFI_MGR_SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_timer notify hg_mcfg
)

if(WIFI_MGR_SRCS)
    idf_component_optional_requires(PRIVATE espressif__mdns)
endif()
