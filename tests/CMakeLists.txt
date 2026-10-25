#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# Fetch Google Test framework.
# Downloaded once during cmake configuration and cached in build/_deps/.
# Subsequent builds work fully offline.
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    # Set extracted file timestamps to extraction time (CMP0135 NEW behavior).
    # Without this, CMake warns on versions >= 3.24 and uses archive timestamps,
    # which can cause incremental builds to miss updates when the URL changes.
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Prevent gtest from overriding compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(unit)

# Integration tests execute instrumented programs and assert on their trace
# output; with DISABLE_INSTRUMENTATION=ON the hooks are compiled out, so the
# whole suite would fail by design. Unit tests (pure header functions,
# unaffected by the flag) still build and run.
if(DISABLE_INSTRUMENTATION)
    message(STATUS "DISABLE_INSTRUMENTATION=ON — skipping integration tests "
            "(they assert on trace output the disabled hooks can never produce)")
    # What the option promises instead: callstacklogger::instrumented adds
    # nothing to a program, and the on-demand API still works through the
    # plain target. tests/disabled/ is a consumer project that builds one
    # program with the instrumented target and one without and compares the
    # binaries (NEEDED libraries, debug sections, exported symbols), plus an
    # api_program linking callstacklogger::callstacklogger whose
    # get_call_stack() output must name its ancestors; it is configured and
    # built as its own Release project, as a user's project would be, so that
    # this tree's own compiler flags do not level the two builds.
    add_test(NAME cslg_disabled_instrumentation_is_inert
        COMMAND ${CMAKE_CTEST_COMMAND} --build-and-test
            ${CMAKE_CURRENT_SOURCE_DIR}/disabled
            ${CMAKE_CURRENT_BINARY_DIR}/disabled-check
            --build-generator ${CMAKE_GENERATOR}
            --build-makeprogram ${CMAKE_MAKE_PROGRAM}
            --build-options
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DCSLG_SOURCE_DIR=${PROJECT_SOURCE_DIR}
                -DFETCHCONTENT_SOURCE_DIR_LIBBACKTRACE=${CSLG_LIBBACKTRACE_SOURCE_DIR}
            --test-command ${CMAKE_CURRENT_SOURCE_DIR}/disabled/run_checks.sh
                ${CMAKE_CURRENT_BINARY_DIR}/disabled-check/with_target
                ${CMAKE_CURRENT_BINARY_DIR}/disabled-check/without_target
                ${CMAKE_CURRENT_BINARY_DIR}/disabled-check/api_program)
else()
    add_subdirectory(integration)
endif()
