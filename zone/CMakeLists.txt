cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/hillgrow.cmake)
# Overridable so the app can be configured for a second target without editing
# this file -- the same guard master/CMakeLists.txt carries, and for the same
# reason: the unconditional set() SHADOWS any -DSDKCONFIG_DEFAULTS= on the
# command line, which makes a second target impossible to build at all. IDF
# appends sdkconfig.defaults.<target> to whatever this names. Zone is ESP32-only
# today, so this changes nothing now; it is here because rescue/ and zone/ were
# left carrying the exact bug Task 1 removed from master/, and a P4 rescue app
# is the branch's headline open gap (spec 11.10) -- whoever builds it should not
# have to rediscover this.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# BOOTLOADER_EXTRA_COMPONENT_DIRS is a build *property*, not a plain CMake variable --
# it must be set via idf_build_set_property() after project.cmake is included (so the
# function exists) and before project() (so the bootloader subproject sees it).
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../bootloader_components/main" APPEND)
project(hillgrow_zone)
hillgrow_host_test_gate(hillgrow_zone.elf)
