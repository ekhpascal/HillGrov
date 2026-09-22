cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/hillgrow.cmake)
# Overridable so the app can be configured for a second target without editing
# this file -- the same guard master/CMakeLists.txt carries, and for the same
# reason: the unconditional set() SHADOWS any -DSDKCONFIG_DEFAULTS= on the
# command line, which makes a second target impossible to build at all. IDF
# appends sdkconfig.defaults.<target> to whatever this names. This is THE
# directory that will next need it: no P4 rescue app is built anywhere today
# (spec 11.10, tools/flash_all.py), so a P4 factory slot is always left
# unwritten, and whoever closes that gap would otherwise hit Task 1's bug again
# here with the same confusing symptom.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# BOOTLOADER_EXTRA_COMPONENT_DIRS is a build *property*, not a plain CMake variable --
# it must be set via idf_build_set_property() after project.cmake is included (so the
# function exists) and before project() (so the bootloader subproject sees it).
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../bootloader_components/main" APPEND)
project(hillgrow_rescue)
# No hillgrow_host_test_gate() call here: rescue is exempt from the pre-build
# host-test gate (task 16 ruling) -- it only links pure components already
# covered by the host suite (rescue_handover/hg_blob, plus board).
