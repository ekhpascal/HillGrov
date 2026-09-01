# Read + enforce version.txt as PROJECT_VER for every app (spec 6.3)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../version.txt" HG_VERSION)
string(STRIP "${HG_VERSION}" HG_VERSION)
if(NOT HG_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "version.txt must be MAJOR.MINOR.PATCH, got '${HG_VERSION}'")
endif()
set(PROJECT_VER "${HG_VERSION}")

# Host-test gate: <app>.elf depends on a green ctest run (spec 8)
function(hillgrow_host_test_gate ELF)
    option(HILLGROW_SKIP_HOST_TESTS "Skip host unit tests" OFF)
    if(HILLGROW_SKIP_HOST_TESTS)
        return()
    endif()
    set(REPO ${CMAKE_CURRENT_LIST_DIR}/..)
    file(GLOB_RECURSE HG_TEST_DEPS ${REPO}/components/*.c ${REPO}/components/*.h ${REPO}/tests/host/*.c ${REPO}/tests/host/*.h)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/host_tests_passed.stamp
        COMMAND ${CMAKE_COMMAND} -S ${REPO}/tests/host -B ${CMAKE_BINARY_DIR}/host_tests -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/host_tests --config Release --parallel
        COMMAND ctest --test-dir ${CMAKE_BINARY_DIR}/host_tests -C Release --output-on-failure
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/host_tests_passed.stamp
        DEPENDS ${HG_TEST_DEPS}
        COMMENT "HillGrow host tests")
    add_custom_target(hillgrow_host_tests DEPENDS ${CMAKE_BINARY_DIR}/host_tests_passed.stamp)
    add_dependencies(${ELF} hillgrow_host_tests)
endfunction()
