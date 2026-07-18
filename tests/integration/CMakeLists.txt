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

# Stripped-caller regression fixture: a shared library whose file-local function
# invokes a callback in instrumented user code. Post-build, strip --strip-all
# removes .symtab and all debug sections (.dynsym survives — it's needed at
# runtime), so the callback's caller address resolves to an object where
# dladdr() finds no symbol and BFD finds no line info — the exact input that
# once made resolve_filename_and_line() loop forever (see StrippedCallerTest).
add_library(stripped_caller SHARED stripped_caller_lib.cpp)
target_compile_features(stripped_caller PRIVATE cxx_std_17)
add_custom_command(TARGET stripped_caller POST_BUILD
    COMMAND strip --strip-all $<TARGET_FILE:stripped_caller>
    COMMENT "Stripping symtab and debug info from stripped_caller")
# Deliberately NOT sanitized (no cslg_apply_sanitizers): the fixture simulates a
# foreign, stripped third-party library — real third-party .so files are not
# sanitizer-instrumented either, and the lib is a trivial passthrough with no
# allocations or synchronization for a sanitizer to check. Sanitizing a SHARED
# library also requires the shared sanitizer runtime (libtsan.so / libasan.so),
# which some distros ship as separate optional packages — keeping the fixture
# clean avoids that portability trap while making the simulation more faithful.

add_executable(stripped_caller_program stripped_caller_program.cpp)
target_compile_features(stripped_caller_program PRIVATE cxx_std_17)
target_link_libraries(stripped_caller_program PRIVATE callstacklogger_instrumented stripped_caller)
cslg_apply_sanitizers(stripped_caller_program)

if(COVERAGE)
    target_compile_options(stripped_caller_program PRIVATE --coverage)
    target_link_options(stripped_caller_program PRIVATE --coverage)
    target_link_options(stripped_caller PRIVATE --coverage)
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
# with one or more of the gated #defines so we can exercise the corresponding code
# path without polluting the main build (the main library is built without these
# flags). cslg_add_flag_variant(<variant> <driver.cpp> <MACRO>...) creates the
# callstacklogger_<variant> static library (library sources recompiled with the
# macros PUBLIC) plus a traced_test_program_<variant> executable built from
# <driver.cpp> with INSTRUMENT_FLAGS and linked against it — mirroring what the
# main callstacklogger + traced_test_program pair does for the default build.
function(cslg_add_flag_variant variant driver)
    add_library(callstacklogger_${variant} STATIC
        ${PROJECT_SOURCE_DIR}/src/callStack.cpp
        ${PROJECT_SOURCE_DIR}/src/trace.cpp)
    target_include_directories(callstacklogger_${variant} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_compile_definitions(callstacklogger_${variant} PUBLIC ${ARGN})
    target_compile_options(callstacklogger_${variant} INTERFACE -g)
    target_link_options(callstacklogger_${variant} INTERFACE -rdynamic)
    target_link_libraries(callstacklogger_${variant} PUBLIC ${CSLG_BFD_LINK_LIBS})

    add_executable(traced_test_program_${variant} ${driver})
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
endfunction()

cslg_add_flag_variant(log_addr traced_program.cpp LOG_ADDR)
cslg_add_flag_variant(log_not_demangled traced_program.cpp LOG_NOT_DEMANGLED)

# LOG_ELAPSED uses a DEDICATED driver (log_elapsed_traced_program.cpp) with
# usleep() sentinels at known depths, so LogElapsedFlagTest can assert specific
# lower bounds on the reported durations.
cslg_add_flag_variant(log_elapsed log_elapsed_traced_program.cpp LOG_ELAPSED)

# Combined-flags variant: all three gated macros defined simultaneously. Exercises
# LOG_ELAPSED's byte-offset math under the line-layout changes introduced by
# LOG_ADDR (prepends "addr: [0x...]" after the tree), proving the placeholder
# position (right after the fixed-width timestamp) is independent of the tail
# layout and that pwrite() never stomps the addr column.
cslg_add_flag_variant(log_elapsed_addr_not_demangled traced_program.cpp
    LOG_ELAPSED LOG_ADDR LOG_NOT_DEMANGLED)

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
    STRIPPED_CALLER_PROGRAM_PATH="$<TARGET_FILE:stripped_caller_program>"
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
