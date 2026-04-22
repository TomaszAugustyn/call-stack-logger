#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# Integration tests — compile a traced program and verify its output.

# Traced programs link callstacklogger_instrumented (adds INSTRUMENT_FLAGS and
# transitively the callstacklogger library, include dir, and -rdynamic).
# Non-instrumented programs link plain callstacklogger — same library, no flags.
add_executable(traced_test_program traced_program.cpp)
target_compile_features(traced_test_program PRIVATE cxx_std_17)
target_link_libraries(traced_test_program PRIVATE callstacklogger_instrumented)

if(COVERAGE)
    target_compile_options(traced_test_program PRIVATE --coverage)
    target_link_options(traced_test_program PRIVATE --coverage)
endif()

# Non-instrumented version of the same program. Links plain callstacklogger so no
# INSTRUMENT_FLAGS are applied. Catches regressions in DISABLE_INSTRUMENTATION
# and split compilation.
add_executable(noninstrumented_test_program traced_program.cpp)
target_compile_features(noninstrumented_test_program PRIVATE cxx_std_17)
target_link_libraries(noninstrumented_test_program PRIVATE callstacklogger)

if(COVERAGE)
    target_compile_options(noninstrumented_test_program PRIVATE --coverage)
    target_link_options(noninstrumented_test_program PRIVATE --coverage)
endif()

# Exercises the public get_call_stack() API (on-demand stack capture).
# Links plain callstacklogger — no per-call hooks needed.
add_executable(callstack_api_program callstack_api_program.cpp)
target_compile_features(callstack_api_program PRIVATE cxx_std_17)
target_link_libraries(callstack_api_program PRIVATE callstacklogger)

if(COVERAGE)
    target_compile_options(callstack_api_program PRIVATE --coverage)
    target_link_options(callstack_api_program PRIVATE --coverage)
endif()

# Multi-threaded traced test program. Exercises per-thread trace files by spawning
# worker threads, each of which produces its own <base>_tid_<tid> file.
find_package(Threads REQUIRED)
add_executable(threaded_traced_test_program threaded_traced_program.cpp)
target_compile_features(threaded_traced_test_program PRIVATE cxx_std_17)
target_link_libraries(threaded_traced_test_program PRIVATE callstacklogger_instrumented Threads::Threads)

if(COVERAGE)
    target_compile_options(threaded_traced_test_program PRIVATE --coverage)
    target_link_options(threaded_traced_test_program PRIVATE --coverage)
endif()

# The integration test runner executes traced_test_program and parses output.
# Compiled WITHOUT instrumentation — just gtest code.
add_executable(integration_tests test_integration.cpp)
target_include_directories(integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(integration_tests PRIVATE cxx_std_17)
target_link_libraries(integration_tests gtest_main)

# Pass the traced program paths so tests can find and execute them
target_compile_definitions(integration_tests PRIVATE
    TRACED_PROGRAM_PATH="$<TARGET_FILE:traced_test_program>"
    NONINSTRUMENTED_PROGRAM_PATH="$<TARGET_FILE:noninstrumented_test_program>"
    THREADED_TRACED_PROGRAM_PATH="$<TARGET_FILE:threaded_traced_test_program>"
    CALLSTACK_API_PROGRAM_PATH="$<TARGET_FILE:callstack_api_program>"
)

if(COVERAGE)
    target_compile_options(integration_tests PRIVATE --coverage)
    target_link_options(integration_tests PRIVATE --coverage)
endif()

add_test(NAME integration_tests COMMAND integration_tests)
