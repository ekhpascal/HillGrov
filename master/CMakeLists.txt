cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/hillgrow.cmake)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# BOOTLOADER_EXTRA_COMPONENT_DIRS is a build *property*, not a plain CMake variable --
# it must be set via idf_build_set_property() after project.cmake is included (so the
# function exists) and before project() (so the bootloader subproject sees it).
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../bootloader_components/main" APPEND)
project(hillgrow_master)
hillgrow_host_test_gate(hillgrow_master.elf)
