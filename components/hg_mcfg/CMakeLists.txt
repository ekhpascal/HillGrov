# mcfg_store.c is the master's NVS glue (nvs_open("hg")/key "mcfg") and must
# NOT be compiled into the zone build, which never touches that key. IDF
# components are configured after the top-level project() call, so
# CMAKE_PROJECT_NAME already names the building app here; see
# master/CMakeLists.txt (project(hillgrow_master)) vs zone/CMakeLists.txt
# (project(hillgrow_zone)).
set(HG_MCFG_SRCS "hg_mcfg.c")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    list(APPEND HG_MCFG_SRCS "mcfg_store.c")
endif()

# PRIV_REQUIRES nvs_flash is harmless on the zone build too: zone already
# links nvs_flash via hg_store, it's just never used from this component
# there because mcfg_store.c isn't in HG_MCFG_SRCS above.
idf_component_register(SRCS ${HG_MCFG_SRCS} INCLUDE_DIRS "." REQUIRES hg_cfg hg_blob PRIV_REQUIRES nvs_flash)
