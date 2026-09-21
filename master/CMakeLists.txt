cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/hillgrow.cmake)
# Overridable so the app can be configured for a second target without editing
# this file. IDF appends sdkconfig.defaults.<target> to whatever this names, so
# the P4 settings live in sdkconfig.defaults.esp32p4 and the ESP32 build is
# unaffected.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# BOOTLOADER_EXTRA_COMPONENT_DIRS is a build *property*, not a plain CMake variable --
# it must be set via idf_build_set_property() after project.cmake is included (so the
# function exists) and before project() (so the bootloader subproject sees it).
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../bootloader_components/main" APPEND)
project(hillgrow_master)
hillgrow_host_test_gate(hillgrow_master.elf)
