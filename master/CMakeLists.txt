cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/hillgrow.cmake)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
set(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../bootloader_components/main")  # consumed in Task 15
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hillgrow_master)
hillgrow_host_test_gate(hillgrow_master.elf)
