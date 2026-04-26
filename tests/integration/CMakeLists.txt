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
cslg_apply_sanitizers(traced_test_program)

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
cslg_apply_sanitizers(noninstrumented_test_program)

if(COVERAGE)
    target_compile_options(noninstrumented_test_program PRIVATE --coverage)
    target_link_options(noninstrumented_test_program PRIVATE --coverage)
endif()

# Exercises the public get_call_stack() API (on-demand stack capture).
# Links plain callstacklogger — no per-call hooks needed.
add_executable(callstack_api_program callstack_api_program.cpp)
target_compile_features(callstack_api_program PRIVATE cxx_std_17)
target_link_libraries(callstack_api_program PRIVATE callstacklogger)
cslg_apply_sanitizers(callstack_api_program)

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
cslg_apply_sanitizers(threaded_traced_test_program)

if(COVERAGE)
    target_compile_options(threaded_traced_test_program PRIVATE --coverage)
    target_link_options(threaded_traced_test_program PRIVATE --coverage)
endif()

# Library variants for testing build-time flags. Each rebuilds the library sources
# with one of the gated #defines so we can exercise the corresponding code path
# without polluting the main build (the main library is built without these flags).
#
# Single-flag variants that reuse the existing traced_program.cpp driver:
foreach(variant IN ITEMS log_addr log_not_demangled)
    string(TOUPPER "${variant}" variant_upper)
    add_library(callstacklogger_${variant} STATIC
        ${PROJECT_SOURCE_DIR}/src/callStack.cpp
        ${PROJECT_SOURCE_DIR}/src/trace.cpp)
    target_include_directories(callstacklogger_${variant} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_compile_definitions(callstacklogger_${variant} PUBLIC ${variant_upper})
    target_compile_options(callstacklogger_${variant} INTERFACE -g)
    target_link_options(callstacklogger_${variant} INTERFACE -rdynamic)
    target_link_libraries(callstacklogger_${variant} PUBLIC -ldl -lbfd)

    add_executable(traced_test_program_${variant} traced_program.cpp)
    target_compile_features(traced_test_program_${variant} PRIVATE cxx_std_17)
    target_compile_options(traced_test_program_${variant} PRIVATE ${INSTRUMENT_FLAGS})
    target_link_libraries(traced_test_program_${variant} PRIVATE callstacklogger_${variant})

    cslg_apply_sanitizers(callstacklogger_${variant})
    cslg_apply_sanitizers(traced_test_program_${variant})

    if(COVERAGE)
        target_compile_options(callstacklogger_${variant} PRIVATE --coverage)
        target_compile_options(traced_test_program_${variant} PRIVATE --coverage)
        target_link_options(traced_test_program_${variant} PRIVATE --coverage)
    endif()
endforeach()

# LOG_ELAPSED variant — uses a DEDICATED driver (log_elapsed_traced_program.cpp)
# with usleep() sentinels at known depths, so LogElapsedFlagTest can assert
# specific lower bounds on the reported durations.
add_library(callstacklogger_log_elapsed STATIC
    ${PROJECT_SOURCE_DIR}/src/callStack.cpp
    ${PROJECT_SOURCE_DIR}/src/trace.cpp)
target_include_directories(callstacklogger_log_elapsed PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_definitions(callstacklogger_log_elapsed PUBLIC LOG_ELAPSED)
target_compile_options(callstacklogger_log_elapsed INTERFACE -g)
target_link_options(callstacklogger_log_elapsed INTERFACE -rdynamic)
target_link_libraries(callstacklogger_log_elapsed PUBLIC -ldl -lbfd)

add_executable(traced_test_program_log_elapsed log_elapsed_traced_program.cpp)
target_compile_features(traced_test_program_log_elapsed PRIVATE cxx_std_17)
target_compile_options(traced_test_program_log_elapsed PRIVATE ${INSTRUMENT_FLAGS})
target_link_libraries(traced_test_program_log_elapsed PRIVATE callstacklogger_log_elapsed)

cslg_apply_sanitizers(callstacklogger_log_elapsed)
cslg_apply_sanitizers(traced_test_program_log_elapsed)

if(COVERAGE)
    target_compile_options(callstacklogger_log_elapsed PRIVATE --coverage)
    target_compile_options(traced_test_program_log_elapsed PRIVATE --coverage)
    target_link_options(traced_test_program_log_elapsed PRIVATE --coverage)
endif()

# Combined-flags variant: all three gated macros defined simultaneously. Exercises
# LOG_ELAPSED's byte-offset math under the line-layout changes introduced by
# LOG_ADDR (prepends "addr: [0x...]" after the tree), proving the placeholder
# position (right after the fixed-width timestamp) is independent of the tail
# layout and that pwrite() never stomps the addr column.
set(combined_variant log_elapsed_addr_not_demangled)
add_library(callstacklogger_${combined_variant} STATIC
    ${PROJECT_SOURCE_DIR}/src/callStack.cpp
    ${PROJECT_SOURCE_DIR}/src/trace.cpp)
target_include_directories(callstacklogger_${combined_variant} PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_definitions(callstacklogger_${combined_variant} PUBLIC
    LOG_ELAPSED LOG_ADDR LOG_NOT_DEMANGLED)
target_compile_options(callstacklogger_${combined_variant} INTERFACE -g)
target_link_options(callstacklogger_${combined_variant} INTERFACE -rdynamic)
target_link_libraries(callstacklogger_${combined_variant} PUBLIC -ldl -lbfd)

add_executable(traced_test_program_${combined_variant} traced_program.cpp)
target_compile_features(traced_test_program_${combined_variant} PRIVATE cxx_std_17)
target_compile_options(traced_test_program_${combined_variant} PRIVATE ${INSTRUMENT_FLAGS})
target_link_libraries(traced_test_program_${combined_variant} PRIVATE
    callstacklogger_${combined_variant})

cslg_apply_sanitizers(callstacklogger_${combined_variant})
cslg_apply_sanitizers(traced_test_program_${combined_variant})

if(COVERAGE)
    target_compile_options(callstacklogger_${combined_variant} PRIVATE --coverage)
    target_compile_options(traced_test_program_${combined_variant} PRIVATE --coverage)
    target_link_options(traced_test_program_${combined_variant} PRIVATE --coverage)
endif()

# The integration test runner executes traced_test_program and parses output.
# Compiled WITHOUT instrumentation — just gtest code.
add_executable(integration_tests test_integration.cpp)
target_include_directories(integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(integration_tests PRIVATE cxx_std_17)
target_link_libraries(integration_tests gtest_main)
cslg_apply_sanitizers(integration_tests)

# Pass the traced program paths so tests can find and execute them
target_compile_definitions(integration_tests PRIVATE
    TRACED_PROGRAM_PATH="$<TARGET_FILE:traced_test_program>"
    NONINSTRUMENTED_PROGRAM_PATH="$<TARGET_FILE:noninstrumented_test_program>"
    THREADED_TRACED_PROGRAM_PATH="$<TARGET_FILE:threaded_traced_test_program>"
    CALLSTACK_API_PROGRAM_PATH="$<TARGET_FILE:callstack_api_program>"
    TRACED_PROGRAM_LOG_ADDR_PATH="$<TARGET_FILE:traced_test_program_log_addr>"
    TRACED_PROGRAM_LOG_NOT_DEMANGLED_PATH="$<TARGET_FILE:traced_test_program_log_not_demangled>"
    TRACED_PROGRAM_LOG_ELAPSED_PATH="$<TARGET_FILE:traced_test_program_log_elapsed>"
    TRACED_PROGRAM_LOG_ELAPSED_COMBINED_PATH="$<TARGET_FILE:traced_test_program_log_elapsed_addr_not_demangled>"
)

# When the user passes -DLOG_ADDR=ON, -DLOG_NOT_DEMANGLED=ON, or -DLOG_ELAPSED=ON
# globally, the default callstacklogger target also defines that macro PUBLIC —
# so tests that expect the DEFAULT build to NOT have the macro must skip themselves
# to avoid false failures.
if(LOG_ADDR)
    target_compile_definitions(integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_ADDR)
endif()
if(LOG_NOT_DEMANGLED)
    target_compile_definitions(integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_NOT_DEMANGLED)
endif()
if(LOG_ELAPSED)
    target_compile_definitions(integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_ELAPSED)
endif()

if(COVERAGE)
    target_compile_options(integration_tests PRIVATE --coverage)
    target_link_options(integration_tests PRIVATE --coverage)
endif()

add_test(NAME integration_tests COMMAND integration_tests)
