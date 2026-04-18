#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# Integration tests — compile a traced program and verify its output.

# The traced test program is compiled WITH instrumentation, just like runDemo.
# It links against callstacklogger and produces trace output when executed.
add_executable(traced_test_program traced_program.cpp)
target_include_directories(traced_test_program PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(traced_test_program PRIVATE cxx_std_17)

# Use the same INSTRUMENT_FLAGS as runDemo (set in root CMakeLists.txt).
target_compile_options(traced_test_program PRIVATE ${INSTRUMENT_FLAGS})

# -rdynamic is set globally in root CMakeLists.txt via add_link_options.
target_link_libraries(traced_test_program callstacklogger)

if(COVERAGE)
    target_compile_options(traced_test_program PRIVATE --coverage)
    target_link_options(traced_test_program PRIVATE --coverage)
endif()

# Non-instrumented version of the same program. Compiled WITHOUT INSTRUMENT_FLAGS
# to verify that no trace output is produced when instrumentation is disabled.
# This catches regressions in DISABLE_INSTRUMENTATION and split compilation.
add_executable(noninstrumented_test_program traced_program.cpp)
target_include_directories(noninstrumented_test_program PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(noninstrumented_test_program PRIVATE cxx_std_17)
# No INSTRUMENT_FLAGS — simulates DISABLE_INSTRUMENTATION=ON
target_link_libraries(noninstrumented_test_program callstacklogger)

if(COVERAGE)
    target_compile_options(noninstrumented_test_program PRIVATE --coverage)
    target_link_options(noninstrumented_test_program PRIVATE --coverage)
endif()

# The integration test runner executes traced_test_program and parses output.
# Compiled WITHOUT instrumentation — just gtest code.
add_executable(integration_tests test_integration.cpp)
target_include_directories(integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(integration_tests PRIVATE cxx_std_17)
target_link_libraries(integration_tests gtest_main)

# Pass the traced program path so tests can find and execute it
target_compile_definitions(integration_tests PRIVATE
    TRACED_PROGRAM_PATH="$<TARGET_FILE:traced_test_program>"
    NONINSTRUMENTED_PROGRAM_PATH="$<TARGET_FILE:noninstrumented_test_program>"
)

if(COVERAGE)
    target_compile_options(integration_tests PRIVATE --coverage)
    target_link_options(integration_tests PRIVATE --coverage)
endif()

add_test(NAME integration_tests COMMAND integration_tests)
