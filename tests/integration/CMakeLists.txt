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

# Non-instrumented version of the same program. Links plain callstacklogger so no
# INSTRUMENT_FLAGS are applied. Catches regressions in DISABLE_INSTRUMENTATION
# and split compilation.
add_executable(noninstrumented_test_program traced_program.cpp)
target_compile_features(noninstrumented_test_program PRIVATE cxx_std_17)
target_link_libraries(noninstrumented_test_program PRIVATE callstacklogger)
cslg_apply_sanitizers(noninstrumented_test_program)

# Threads is needed by the api programs (concurrent worker-thread captures)
# and by the threaded traced programs further down.
find_package(Threads REQUIRED)

# Exercises the public get_call_stack() API (on-demand stack capture), from
# the main thread and from two concurrently-capturing worker threads.
# Links plain callstacklogger — no per-call hooks needed.
add_executable(callstack_api_program callstack_api_program.cpp)
target_compile_features(callstack_api_program PRIVATE cxx_std_17)
target_link_libraries(callstack_api_program PRIVATE callstacklogger Threads::Threads)
# Keep every chain frame physically on the stack in optimized suite builds:
# backtrace() cannot see frames removed by sibling-call optimization, and the
# test asserts the exact ancestor chain. Complements the noinline attributes
# in the driver source (which only prevent inlining, not tail calls).
target_compile_options(callstack_api_program PRIVATE -fno-optimize-sibling-calls)
cslg_apply_sanitizers(callstack_api_program)

# Same source as callstack_api_program, but compiled WITH instrumentation and
# linked against callstacklogger_instrumented — proves the on-demand
# get_call_stack() API and the per-call hooks coexist in one process (the API
# takes s_bfd_mutex outside any hook while hooks fire around it).
add_executable(callstack_api_program_instrumented callstack_api_program.cpp)
target_compile_features(callstack_api_program_instrumented PRIVATE cxx_std_17)
target_link_libraries(callstack_api_program_instrumented PRIVATE callstacklogger_instrumented
    Threads::Threads)
cslg_apply_sanitizers(callstack_api_program_instrumented)

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

# Deep-recursion driver: recurses past MAX_TRACE_DEPTH (2048) so the overflow
# counter in trace.cpp is exercised end-to-end (see OverflowDepthTest).
add_executable(overflow_depth_program overflow_depth_program.cpp)
target_compile_features(overflow_depth_program PRIVATE cxx_std_17)
target_link_libraries(overflow_depth_program PRIVATE callstacklogger_instrumented)
cslg_apply_sanitizers(overflow_depth_program)

# Exception-unwind driver: throws through instrumented frames. Only GCC emits
# exit hooks on the unwind path, so the matching test asserts under GCC only
# (see ExceptionUnwindTest and the CSLG_COMPILER_IS_GNU definition below).
add_executable(exception_traced_program exception_traced_program.cpp)
target_compile_features(exception_traced_program PRIVATE cxx_std_17)
target_link_libraries(exception_traced_program PRIVATE callstacklogger_instrumented)
cslg_apply_sanitizers(exception_traced_program)

# Exit-window driver: a global object's instrumented destructor runs during
# exit(). Its construction (pre-trace_begin) and destruction
# (post-trace_shutdown) both hit disabled hooks and must be silent no-ops —
# most valuable under the sanitizer jobs, which would catch any exit-time
# use-after-free (see GlobalDtorTest).
add_executable(global_dtor_traced_program global_dtor_traced_program.cpp)
target_compile_features(global_dtor_traced_program PRIVATE cxx_std_17)
target_link_libraries(global_dtor_traced_program PRIVATE callstacklogger_instrumented)
cslg_apply_sanitizers(global_dtor_traced_program)

# Multi-threaded traced test program. Exercises per-thread trace files by spawning
# worker threads, each of which produces its own <base>_tid_<tid> file.
add_executable(threaded_traced_test_program threaded_traced_program.cpp)
target_compile_features(threaded_traced_test_program PRIVATE cxx_std_17)
target_link_libraries(threaded_traced_test_program PRIVATE callstacklogger_instrumented Threads::Threads)
cslg_apply_sanitizers(threaded_traced_test_program)

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
    # Mirror the main library's private 64-bit off_t setting (src/CMakeLists.txt)
    # so the variant libraries compile the same code the shipped library does.
    target_compile_definitions(callstacklogger_${variant} PRIVATE _FILE_OFFSET_BITS=64)
    target_compile_options(callstacklogger_${variant} INTERFACE -g)
    target_link_options(callstacklogger_${variant} INTERFACE -rdynamic)
    target_link_libraries(callstacklogger_${variant} PUBLIC ${CSLG_BFD_LINK_LIBS})

    add_executable(traced_test_program_${variant} ${driver})
    target_compile_features(traced_test_program_${variant} PRIVATE cxx_std_17)
    target_compile_options(traced_test_program_${variant} PRIVATE ${INSTRUMENT_FLAGS})
    target_link_libraries(traced_test_program_${variant} PRIVATE callstacklogger_${variant})

    cslg_apply_sanitizers(callstacklogger_${variant})
    cslg_apply_sanitizers(traced_test_program_${variant})
endfunction()

cslg_add_flag_variant(log_addr traced_program.cpp LOG_ADDR)
cslg_add_flag_variant(log_not_demangled traced_program.cpp LOG_NOT_DEMANGLED)

# LOG_ELAPSED uses a DEDICATED driver (log_elapsed_traced_program.cpp) with
# usleep() sentinels at known depths, so LogElapsedFlagTest can assert specific
# lower bounds on the reported durations.
cslg_add_flag_variant(log_elapsed log_elapsed_traced_program.cpp LOG_ELAPSED)

# LOG_ELAPSED + multi-threading: the threaded driver against the LOG_ELAPSED
# library variant, proving each thread's cursor / patch_fd patches its own file
# independently (per-thread placeholder offsets never cross-contaminate).
cslg_add_flag_variant(log_elapsed_threaded threaded_traced_program.cpp LOG_ELAPSED)
target_link_libraries(traced_test_program_log_elapsed_threaded PRIVATE Threads::Threads)

# Crash-diagnostics driver against the SAME log_elapsed variant library (no new
# library build needed): aborts mid-chain, so LogElapsedCrashTest can assert
# that the frames active at the crash keep their "[  pending ]" placeholders
# while an already-completed frame's line carries a real patched duration —
# pins README's "Crash diagnostics" feature end-to-end.
add_executable(crash_traced_program_log_elapsed crash_traced_program.cpp)
target_compile_features(crash_traced_program_log_elapsed PRIVATE cxx_std_17)
target_compile_options(crash_traced_program_log_elapsed PRIVATE ${INSTRUMENT_FLAGS})
target_link_libraries(crash_traced_program_log_elapsed PRIVATE callstacklogger_log_elapsed)
cslg_apply_sanitizers(crash_traced_program_log_elapsed)

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
    CALLSTACK_API_PROGRAM_INSTRUMENTED_PATH="$<TARGET_FILE:callstack_api_program_instrumented>"
    STRIPPED_CALLER_PROGRAM_PATH="$<TARGET_FILE:stripped_caller_program>"
    OVERFLOW_DEPTH_PROGRAM_PATH="$<TARGET_FILE:overflow_depth_program>"
    EXCEPTION_TRACED_PROGRAM_PATH="$<TARGET_FILE:exception_traced_program>"
    GLOBAL_DTOR_PROGRAM_PATH="$<TARGET_FILE:global_dtor_traced_program>"
    CRASH_PROGRAM_LOG_ELAPSED_PATH="$<TARGET_FILE:crash_traced_program_log_elapsed>"
    TRACED_PROGRAM_LOG_ADDR_PATH="$<TARGET_FILE:traced_test_program_log_addr>"
    TRACED_PROGRAM_LOG_NOT_DEMANGLED_PATH="$<TARGET_FILE:traced_test_program_log_not_demangled>"
    TRACED_PROGRAM_LOG_ELAPSED_PATH="$<TARGET_FILE:traced_test_program_log_elapsed>"
    THREADED_TRACED_PROGRAM_LOG_ELAPSED_PATH="$<TARGET_FILE:traced_test_program_log_elapsed_threaded>"
    TRACED_PROGRAM_LOG_ELAPSED_COMBINED_PATH="$<TARGET_FILE:traced_test_program_log_elapsed_addr_not_demangled>"
)

# When the user passes -DLOG_ADDR=ON, -DLOG_NOT_DEMANGLED=ON, or -DLOG_ELAPSED=ON
# globally, the default callstacklogger target also defines that macro PUBLIC —
# so tests that expect the DEFAULT build to NOT have the macro must skip themselves
# to avoid false failures.
if(LOG_ADDR)
    target_compile_definitions(integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_ADDR)
endif()
# GCC emits __cyg_profile_func_exit on the exception-unwind path; Clang does not
# (documented limitation). ExceptionUnwindTest asserts only when this is set.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_definitions(integration_tests PRIVATE CSLG_COMPILER_IS_GNU)
endif()
if(LOG_NOT_DEMANGLED)
    target_compile_definitions(integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_NOT_DEMANGLED)
endif()
if(LOG_ELAPSED)
    target_compile_definitions(integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_ELAPSED)
endif()

add_test(NAME integration_tests COMMAND integration_tests)
