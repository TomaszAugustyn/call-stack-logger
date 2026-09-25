#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# Integration tests — compile a traced program and verify its output.
# Every target and ctest name carries a cslg_ prefix so nothing here can clash
# with a parent project's targets (the test tree is only added for top-level
# builds anyway, but the prefix keeps the names unambiguous in ctest output too).

# Traced programs link callstacklogger_instrumented (adds INSTRUMENT_FLAGS and
# transitively the callstacklogger library, include dir, and -rdynamic).
# Non-instrumented programs link plain callstacklogger — same library, no flags.
add_executable(cslg_traced_test_program traced_program.cpp)
target_compile_features(cslg_traced_test_program PRIVATE cxx_std_17)
target_link_libraries(cslg_traced_test_program PRIVATE callstacklogger_instrumented)
cslg_apply_sanitizers(cslg_traced_test_program)

# Non-instrumented version of the same program. Links plain callstacklogger so no
# INSTRUMENT_FLAGS are applied. Catches regressions in DISABLE_INSTRUMENTATION
# and split compilation.
add_executable(cslg_noninstrumented_test_program traced_program.cpp)
target_compile_features(cslg_noninstrumented_test_program PRIVATE cxx_std_17)
target_link_libraries(cslg_noninstrumented_test_program PRIVATE callstacklogger)
cslg_apply_sanitizers(cslg_noninstrumented_test_program)

# Threads is needed by the api programs (concurrent worker-thread captures)
# and by the threaded traced programs further down.
find_package(Threads REQUIRED)

# Exercises the public get_call_stack() API (on-demand stack capture), from
# the main thread and from two concurrently-capturing worker threads.
# Links plain callstacklogger — no per-call hooks needed.
add_executable(cslg_callstack_api_program callstack_api_program.cpp)
target_compile_features(cslg_callstack_api_program PRIVATE cxx_std_17)
target_link_libraries(cslg_callstack_api_program PRIVATE callstacklogger Threads::Threads)
# Keep every chain frame physically on the stack in optimized suite builds:
# backtrace() cannot see frames removed by sibling-call optimization, and the
# test asserts the exact ancestor chain. Complements the noinline attributes
# in the driver source (which only prevent inlining, not tail calls).
target_compile_options(cslg_callstack_api_program PRIVATE -fno-optimize-sibling-calls)
cslg_apply_sanitizers(cslg_callstack_api_program)

# Same source as cslg_callstack_api_program, but compiled WITH instrumentation and
# linked against callstacklogger_instrumented — proves the on-demand
# get_call_stack() API and the per-call hooks coexist in one process (the API
# takes s_bfd_mutex outside any hook while hooks fire around it).
add_executable(cslg_callstack_api_program_instrumented callstack_api_program.cpp)
target_compile_features(cslg_callstack_api_program_instrumented PRIVATE cxx_std_17)
target_link_libraries(cslg_callstack_api_program_instrumented PRIVATE callstacklogger_instrumented
    Threads::Threads)
cslg_apply_sanitizers(cslg_callstack_api_program_instrumented)

# Stripped-caller regression fixture: a shared library whose file-local function
# invokes a callback in instrumented user code. Post-build, strip --strip-all
# removes .symtab and all debug sections (.dynsym survives — it's needed at
# runtime), so the callback's caller address resolves to an object where
# dladdr() finds no symbol and BFD finds no line info — the exact input that
# once made resolve_filename_and_line() loop forever (see StrippedCallerTest).
add_library(cslg_stripped_caller SHARED stripped_caller_lib.cpp)
target_compile_features(cslg_stripped_caller PRIVATE cxx_std_17)
add_custom_command(TARGET cslg_stripped_caller POST_BUILD
    COMMAND strip --strip-all $<TARGET_FILE:cslg_stripped_caller>
    COMMENT "Stripping symtab and debug info from cslg_stripped_caller")
# Deliberately NOT sanitized (no cslg_apply_sanitizers): the fixture simulates a
# foreign, stripped third-party library — real third-party .so files are not
# sanitizer-instrumented either, and the lib is a trivial passthrough with no
# allocations or synchronization for a sanitizer to check. Sanitizing a SHARED
# library also requires the shared sanitizer runtime (libtsan.so / libasan.so),
# which some distros ship as separate optional packages — keeping the fixture
# clean avoids that portability trap while making the simulation more faithful.

add_executable(cslg_stripped_caller_program stripped_caller_program.cpp)
target_compile_features(cslg_stripped_caller_program PRIVATE cxx_std_17)
target_link_libraries(cslg_stripped_caller_program PRIVATE callstacklogger_instrumented cslg_stripped_caller)
cslg_apply_sanitizers(cslg_stripped_caller_program)

# dlopen()-constructor deadlock fixture: an INSTRUMENTED plugin (INSTRUMENT_FLAGS
# plus -g; it links no cslg target — its hooks bind at load time to the host's
# exported __cyg_profile_func_* symbols) whose static initializer fires the enter
# hook on the dlopen() thread, i.e. with glibc's loader lock held. The driver
# dlopen()s/dlclose()s it in a loop while another thread resolves thousands of
# never-seen addresses. Before the fix the resolver called dladdr() (which takes
# the loader lock) with s_bfd_mutex held and the two threads deadlocked; the test
# runs the driver under `timeout` (see DlopenConstructorTest).
add_library(cslg_dlopen_ctor_plugin SHARED dlopen_ctor_plugin.cpp)
target_compile_features(cslg_dlopen_ctor_plugin PRIVATE cxx_std_17)
target_compile_options(cslg_dlopen_ctor_plugin PRIVATE ${INSTRUMENT_FLAGS} -g)

add_executable(cslg_dlopen_ctor_traced_program dlopen_ctor_traced_program.cpp)
target_compile_features(cslg_dlopen_ctor_traced_program PRIVATE cxx_std_17)
target_compile_definitions(cslg_dlopen_ctor_traced_program PRIVATE
    DLOPEN_CTOR_PLUGIN_PATH="$<TARGET_FILE:cslg_dlopen_ctor_plugin>")
target_link_libraries(cslg_dlopen_ctor_traced_program PRIVATE callstacklogger_instrumented Threads::Threads)
add_dependencies(cslg_dlopen_ctor_traced_program cslg_dlopen_ctor_plugin)
cslg_apply_sanitizers(cslg_dlopen_ctor_traced_program)
# dlopen()/chdir() regression fixture: an INSTRUMENTED plugin shared library
# (INSTRUMENT_FLAGS, plus -g for line info — it links no cslg target, so it
# inherits neither) whose hooks bind at load time to the host executable's
# exported __cyg_profile_func_* symbols (-rdynamic). The driver dlopen()s it
# by a RELATIVE path and chdir()s away before the first traced call into it,
# so the resolver must locate the object file via /proc/self/maps — dladdr's
# relative dli_fname no longer names it (see DlopenPluginTest). Like
# cslg_stripped_caller, deliberately NOT sanitized: a plain passthrough .so with
# no allocations or synchronization, kept free of the shared-runtime
# portability trap.
add_library(cslg_dlopen_plugin SHARED dlopen_plugin.cpp)
target_compile_features(cslg_dlopen_plugin PRIVATE cxx_std_17)
target_compile_options(cslg_dlopen_plugin PRIVATE ${INSTRUMENT_FLAGS} -g)

add_executable(cslg_dlopen_traced_program dlopen_traced_program.cpp)
target_compile_features(cslg_dlopen_traced_program PRIVATE cxx_std_17)
target_compile_definitions(cslg_dlopen_traced_program PRIVATE
    DLOPEN_PLUGIN_DIR="$<TARGET_FILE_DIR:cslg_dlopen_plugin>"
    DLOPEN_PLUGIN_FILE="$<TARGET_FILE_NAME:cslg_dlopen_plugin>")
target_link_libraries(cslg_dlopen_traced_program PRIVATE callstacklogger_instrumented)
# Loaded at runtime only, so no link dependency — make sure it is built anyway.
add_dependencies(cslg_dlopen_traced_program cslg_dlopen_plugin)
cslg_apply_sanitizers(cslg_dlopen_traced_program)

# Deep-recursion driver: recurses past the frame stack's initial capacity
# (INITIAL_FRAME_CAPACITY = 2048 in trace.cpp) so on-demand growth is exercised
# end-to-end (see OverflowDepthTest).
add_executable(cslg_overflow_depth_program overflow_depth_program.cpp)
target_compile_features(cslg_overflow_depth_program PRIVATE cxx_std_17)
target_link_libraries(cslg_overflow_depth_program PRIVATE callstacklogger_instrumented)
cslg_apply_sanitizers(cslg_overflow_depth_program)

# Same recursion, but with an unlogged frame at every level: an instrumented
# shared library, stripped after the build like cslg_stripped_caller, whose
# file-local helper has no symbol left for dladdr() or BFD to name — its enter
# hook fires but the resolver drops the frame. Frames beyond the initial
# capacity are then a mix of logged and unlogged, and the depth accounting must
# stay exact regardless (see FilteredOverflowTest). Not sanitized, for the same
# reasons as cslg_stripped_caller.
add_library(cslg_filtered_overflow_lib SHARED filtered_overflow_lib.cpp)
target_compile_features(cslg_filtered_overflow_lib PRIVATE cxx_std_17)
target_compile_options(cslg_filtered_overflow_lib PRIVATE ${INSTRUMENT_FLAGS})
add_custom_command(TARGET cslg_filtered_overflow_lib POST_BUILD
    COMMAND strip --strip-all $<TARGET_FILE:cslg_filtered_overflow_lib>
    COMMENT "Stripping symtab and debug info from cslg_filtered_overflow_lib")

add_executable(cslg_filtered_overflow_program filtered_overflow_program.cpp)
target_compile_features(cslg_filtered_overflow_program PRIVATE cxx_std_17)
target_link_libraries(cslg_filtered_overflow_program PRIVATE callstacklogger_instrumented cslg_filtered_overflow_lib)
cslg_apply_sanitizers(cslg_filtered_overflow_program)

# Exception-unwind driver: throws through instrumented frames. GCC runs the exit
# hooks of the unwound frames, Clang runs none; the frame reconciliation reclaims
# their records either way, so the matching test asserts on both compilers (see
# ExceptionUnwindTest).
add_executable(cslg_exception_traced_program exception_traced_program.cpp)
target_compile_features(cslg_exception_traced_program PRIVATE cxx_std_17)
target_link_libraries(cslg_exception_traced_program PRIVATE callstacklogger_instrumented)
cslg_apply_sanitizers(cslg_exception_traced_program)

# longjmp driver: jumps over instrumented frames, whose exit hooks then never run
# on either compiler, and calls at the jumper's level afterwards; the frame
# reconciliation puts those calls at their true depth (see LongjmpTest).
add_executable(cslg_longjmp_traced_program longjmp_traced_program.cpp)
target_compile_features(cslg_longjmp_traced_program PRIVATE cxx_std_17)
target_link_libraries(cslg_longjmp_traced_program PRIVATE callstacklogger_instrumented)
cslg_apply_sanitizers(cslg_longjmp_traced_program)

# Throwing-library fixture for the LOG_EXCEPTIONS driver: a shared library built
# WITHOUT instrumentation whose only function throws. The exception is born in
# code the tracer never instrumented and is caught by an instrumented frame —
# the shape of an exception escaping a third-party library. Not sanitized, for
# the same reasons as cslg_stripped_caller.
add_library(cslg_throwing_lib SHARED throwing_lib.cpp)
target_compile_features(cslg_throwing_lib PRIVATE cxx_std_17)

# Exit-window driver: a global object's instrumented destructor runs during
# exit(). Its construction (pre-trace_begin) and destruction
# (post-trace_shutdown) both hit disabled hooks and must be silent no-ops —
# most valuable under the sanitizer jobs, which would catch any exit-time
# use-after-free (see GlobalDtorTest).
add_executable(cslg_global_dtor_traced_program global_dtor_traced_program.cpp)
target_compile_features(cslg_global_dtor_traced_program PRIVATE cxx_std_17)
target_link_libraries(cslg_global_dtor_traced_program PRIVATE callstacklogger_instrumented)
cslg_apply_sanitizers(cslg_global_dtor_traced_program)

# pthread_cancel driver: a worker loops on a traced leaf while main cancels it.
# The hooks contain cancellation points and the compiler emits the hook call as
# non-throwing, so a cancellation landing inside a hook used to abort the whole
# process; the hooks now run with cancellation disabled (see PthreadCancelTest).
add_executable(cslg_cancel_traced_program cancel_traced_program.cpp)
target_compile_features(cslg_cancel_traced_program PRIVATE cxx_std_17)
target_link_libraries(cslg_cancel_traced_program PRIVATE callstacklogger_instrumented Threads::Threads)
cslg_apply_sanitizers(cslg_cancel_traced_program)

# Multi-threaded traced test program. Exercises per-thread trace files by spawning
# worker threads, each of which produces its own <base>_tid_<tid> file.
add_executable(cslg_threaded_traced_test_program threaded_traced_program.cpp)
target_compile_features(cslg_threaded_traced_test_program PRIVATE cxx_std_17)
target_link_libraries(cslg_threaded_traced_test_program PRIVATE callstacklogger_instrumented Threads::Threads)
cslg_apply_sanitizers(cslg_threaded_traced_test_program)

# chdir() driver: switches working directory between the main thread's lazy
# open and a worker thread's, with a RELATIVE CSLG_OUTPUT_FILE. Pins that the
# base path is anchored to the startup directory once in trace_begin(), so all
# per-thread files of one run land together (see ChdirTest).
add_executable(cslg_chdir_traced_program chdir_traced_program.cpp)
target_compile_features(cslg_chdir_traced_program PRIVATE cxx_std_17)
target_link_libraries(cslg_chdir_traced_program PRIVATE callstacklogger_instrumented Threads::Threads)
cslg_apply_sanitizers(cslg_chdir_traced_program)

# Library variants for testing build-time flags. Each rebuilds the library sources
# with one or more of the gated #defines so we can exercise the corresponding code
# path without polluting the main build (the main library is built without these
# flags). cslg_add_flag_variant(<variant> <driver.cpp> <MACRO>...) creates the
# callstacklogger_<variant> static library (library sources recompiled with the
# macros PRIVATE, as the shipped library does) plus a cslg_traced_test_program_<variant>
# executable built from
# <driver.cpp> with INSTRUMENT_FLAGS and linked against it — mirroring what the
# main callstacklogger + cslg_traced_test_program pair does for the default build.
function(cslg_add_flag_variant variant driver)
    add_library(callstacklogger_${variant} STATIC
        ${PROJECT_SOURCE_DIR}/src/callStack.cpp
        ${PROJECT_SOURCE_DIR}/src/trace.cpp
        ${PROJECT_SOURCE_DIR}/src/exceptions.cpp)
    target_include_directories(callstacklogger_${variant} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_compile_definitions(callstacklogger_${variant} PRIVATE ${ARGN})
    # Mirror the main library's private 64-bit off_t setting (src/CMakeLists.txt)
    # so the variant libraries compile the same code the shipped library does.
    target_compile_definitions(callstacklogger_${variant} PRIVATE _FILE_OFFSET_BITS=64)
    target_compile_options(callstacklogger_${variant} INTERFACE -g)
    # Same DWARF 4 request as the shipped library makes under Clang with
    # LOG_EXCEPTIONS (see src/CMakeLists.txt for why).
    if("LOG_EXCEPTIONS" IN_LIST ARGN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(callstacklogger_${variant} INTERFACE -gdwarf-4)
    endif()
    target_link_options(callstacklogger_${variant} INTERFACE -rdynamic)
    target_link_libraries(callstacklogger_${variant} PUBLIC ${CSLG_BFD_LINK_LIBS} Threads::Threads)

    add_executable(cslg_traced_test_program_${variant} ${driver})
    target_compile_features(cslg_traced_test_program_${variant} PRIVATE cxx_std_17)
    target_compile_options(cslg_traced_test_program_${variant} PRIVATE ${INSTRUMENT_FLAGS})
    target_link_libraries(cslg_traced_test_program_${variant} PRIVATE callstacklogger_${variant})

    cslg_apply_sanitizers(callstacklogger_${variant})
    cslg_apply_sanitizers(cslg_traced_test_program_${variant})
endfunction()

cslg_add_flag_variant(log_addr traced_program.cpp LOG_ADDR)

# LOG_ELAPSED uses a DEDICATED driver (log_elapsed_traced_program.cpp) with
# usleep() sentinels at known depths, so LogElapsedFlagTest can assert specific
# lower bounds on the reported durations.
cslg_add_flag_variant(log_elapsed log_elapsed_traced_program.cpp LOG_ELAPSED)

# LOG_ELAPSED + multi-threading: the threaded driver against the LOG_ELAPSED
# library variant, proving each thread's cursor / patch_fd patches its own file
# independently (per-thread placeholder offsets never cross-contaminate).
cslg_add_flag_variant(log_elapsed_threaded threaded_traced_program.cpp LOG_ELAPSED)
target_link_libraries(cslg_traced_test_program_log_elapsed_threaded PRIVATE Threads::Threads)

# Crash-diagnostics driver against the SAME log_elapsed variant library (no new
# library build needed): aborts mid-chain, so LogElapsedCrashTest can assert
# that the frames active at the crash keep their "[  pending ]" placeholders
# while an already-completed frame's line carries a real patched duration —
# pins README's "Crash diagnostics" feature end-to-end.
add_executable(cslg_crash_traced_program_log_elapsed crash_traced_program.cpp)
target_compile_features(cslg_crash_traced_program_log_elapsed PRIVATE cxx_std_17)
target_compile_options(cslg_crash_traced_program_log_elapsed PRIVATE ${INSTRUMENT_FLAGS})
target_link_libraries(cslg_crash_traced_program_log_elapsed PRIVATE callstacklogger_log_elapsed)
cslg_apply_sanitizers(cslg_crash_traced_program_log_elapsed)

# Uncaught-exception driver against the LOG_EXCEPTIONS + LOG_ELAPSED variant
# library: throws from the bottom of a chain and never catches, so std::terminate
# aborts the process. The throw line must be the last line of the trace and every
# frame active at that point must keep its "[  pending ]" placeholder (see
# LogExceptionsUncaughtTest).
add_executable(cslg_uncaught_traced_program_log_exceptions_elapsed uncaught_traced_program.cpp)
target_compile_features(cslg_uncaught_traced_program_log_exceptions_elapsed PRIVATE cxx_std_17)
target_compile_options(cslg_uncaught_traced_program_log_exceptions_elapsed PRIVATE ${INSTRUMENT_FLAGS})
target_link_libraries(cslg_uncaught_traced_program_log_exceptions_elapsed PRIVATE callstacklogger_log_exceptions_elapsed)
cslg_apply_sanitizers(cslg_uncaught_traced_program_log_exceptions_elapsed)

# The cancel driver against the LOG_ELAPSED variant: the exit hook's pwrite and the
# thread-exit close() of the patch descriptor are cancellation points of their own
# (see PthreadCancelTest).
add_executable(cslg_cancel_traced_program_log_elapsed cancel_traced_program.cpp)
target_compile_features(cslg_cancel_traced_program_log_elapsed PRIVATE cxx_std_17)
target_compile_options(cslg_cancel_traced_program_log_elapsed PRIVATE ${INSTRUMENT_FLAGS})
target_link_libraries(cslg_cancel_traced_program_log_elapsed PRIVATE callstacklogger_log_elapsed Threads::Threads)
cslg_apply_sanitizers(cslg_cancel_traced_program_log_elapsed)

# LOG_EXCEPTIONS uses a DEDICATED driver (exceptions_traced_program.cpp) whose
# scenarios end in frames that leave without an exit hook: exceptions caught
# above instrumented frames (no exit hooks on Clang's unwind path), longjmp
# over instrumented frames (no exit hooks on either compiler), plus recursion
# and a throw from a shared library built without instrumentation. Every
# scenario ends with a marker call whose depth the tests assert exactly.
cslg_add_flag_variant(log_exceptions exceptions_traced_program.cpp LOG_EXCEPTIONS)
target_link_libraries(cslg_traced_test_program_log_exceptions PRIVATE Threads::Threads cslg_throwing_lib)

# The same driver with LOG_ELAPSED: the end-of-frame markers then also flag the
# duration field ("[!  1.234ms]", "[~ unwound ]"), and every dead frame's
# placeholder must end up patched, never left "[  pending ]".
cslg_add_flag_variant(log_exceptions_elapsed exceptions_traced_program.cpp LOG_EXCEPTIONS LOG_ELAPSED)
target_link_libraries(cslg_traced_test_program_log_exceptions_elapsed PRIVATE Threads::Threads cslg_throwing_lib)

# ... and with every output-content macro on: the glyph offset must account for
# both the duration column and the address column.
cslg_add_flag_variant(log_exceptions_all exceptions_traced_program.cpp LOG_EXCEPTIONS LOG_ELAPSED LOG_ADDR)
target_link_libraries(cslg_traced_test_program_log_exceptions_all PRIVATE Threads::Threads cslg_throwing_lib)

# Inlined-frames driver: the throwing chain and the retry-loop leaf are
# always_inline, so they share their host's physical frame (and level) at any
# build type — the shape the level rules alone cannot reconcile and the DWARF
# inline chain must. Compiled at -O2 unconditionally so the inlining decisions
# and hook placement match the optimized builds the README recommends.
cslg_add_flag_variant(log_exceptions_inlined inlined_traced_program.cpp LOG_EXCEPTIONS)
target_compile_options(cslg_traced_test_program_log_exceptions_inlined PRIVATE -O2)

# Combined-flags variant: both output-content macros defined simultaneously.
# Exercises LOG_ELAPSED's byte-offset math under the line-layout changes
# introduced by LOG_ADDR (prepends "addr: [0x...]" after the tree), proving the
# placeholder position (right after the fixed-width timestamp) is independent
# of the tail layout and that pwrite() never stomps the addr column.
cslg_add_flag_variant(log_elapsed_addr traced_program.cpp LOG_ELAPSED LOG_ADDR)

# The integration test runner executes cslg_traced_test_program and parses output.
# Compiled WITHOUT instrumentation — just gtest code.
add_executable(cslg_integration_tests test_integration.cpp)
target_include_directories(cslg_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(cslg_integration_tests PRIVATE cxx_std_17)
target_link_libraries(cslg_integration_tests gtest_main)
cslg_apply_sanitizers(cslg_integration_tests)

# Pass the traced program paths so tests can find and execute them
target_compile_definitions(cslg_integration_tests PRIVATE
    TRACED_PROGRAM_PATH="$<TARGET_FILE:cslg_traced_test_program>"
    NONINSTRUMENTED_PROGRAM_PATH="$<TARGET_FILE:cslg_noninstrumented_test_program>"
    THREADED_TRACED_PROGRAM_PATH="$<TARGET_FILE:cslg_threaded_traced_test_program>"
    CALLSTACK_API_PROGRAM_PATH="$<TARGET_FILE:cslg_callstack_api_program>"
    CALLSTACK_API_PROGRAM_INSTRUMENTED_PATH="$<TARGET_FILE:cslg_callstack_api_program_instrumented>"
    STRIPPED_CALLER_PROGRAM_PATH="$<TARGET_FILE:cslg_stripped_caller_program>"
    DLOPEN_CTOR_PROGRAM_PATH="$<TARGET_FILE:cslg_dlopen_ctor_traced_program>"
    DLOPEN_TRACED_PROGRAM_PATH="$<TARGET_FILE:cslg_dlopen_traced_program>"
    OVERFLOW_DEPTH_PROGRAM_PATH="$<TARGET_FILE:cslg_overflow_depth_program>"
    FILTERED_OVERFLOW_PROGRAM_PATH="$<TARGET_FILE:cslg_filtered_overflow_program>"
    EXCEPTION_TRACED_PROGRAM_PATH="$<TARGET_FILE:cslg_exception_traced_program>"
    LONGJMP_TRACED_PROGRAM_PATH="$<TARGET_FILE:cslg_longjmp_traced_program>"
    GLOBAL_DTOR_PROGRAM_PATH="$<TARGET_FILE:cslg_global_dtor_traced_program>"
    CANCEL_TRACED_PROGRAM_PATH="$<TARGET_FILE:cslg_cancel_traced_program>"
    CANCEL_PROGRAM_LOG_ELAPSED_PATH="$<TARGET_FILE:cslg_cancel_traced_program_log_elapsed>"
    CHDIR_TRACED_PROGRAM_PATH="$<TARGET_FILE:cslg_chdir_traced_program>"
    CRASH_PROGRAM_LOG_ELAPSED_PATH="$<TARGET_FILE:cslg_crash_traced_program_log_elapsed>"
    TRACED_PROGRAM_LOG_ADDR_PATH="$<TARGET_FILE:cslg_traced_test_program_log_addr>"
    TRACED_PROGRAM_LOG_ELAPSED_PATH="$<TARGET_FILE:cslg_traced_test_program_log_elapsed>"
    THREADED_TRACED_PROGRAM_LOG_ELAPSED_PATH="$<TARGET_FILE:cslg_traced_test_program_log_elapsed_threaded>"
    TRACED_PROGRAM_LOG_ELAPSED_COMBINED_PATH="$<TARGET_FILE:cslg_traced_test_program_log_elapsed_addr>"
    TRACED_PROGRAM_LOG_EXCEPTIONS_PATH="$<TARGET_FILE:cslg_traced_test_program_log_exceptions>"
    TRACED_PROGRAM_LOG_EXCEPTIONS_ELAPSED_PATH="$<TARGET_FILE:cslg_traced_test_program_log_exceptions_elapsed>"
    TRACED_PROGRAM_LOG_EXCEPTIONS_ALL_PATH="$<TARGET_FILE:cslg_traced_test_program_log_exceptions_all>"
    UNCAUGHT_PROGRAM_LOG_EXCEPTIONS_ELAPSED_PATH="$<TARGET_FILE:cslg_uncaught_traced_program_log_exceptions_elapsed>"
    TRACED_PROGRAM_LOG_EXCEPTIONS_INLINED_PATH="$<TARGET_FILE:cslg_traced_test_program_log_exceptions_inlined>"
)

# When the user passes -DLOG_ADDR=ON or -DLOG_ELAPSED=ON globally, the
# default callstacklogger target also defines that macro PUBLIC —
# so tests that expect the DEFAULT build to NOT have the macro must skip themselves
# to avoid false failures.
if(LOG_ADDR)
    target_compile_definitions(cslg_integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_ADDR)
endif()
# GCC runs the exit hooks of the frames an exception unwinds through (so their
# lines get a measured, marked duration); Clang runs none, and the reconciliation
# reclaims their records at the catch instead. The LOG_EXCEPTIONS tests expect
# the marks that follow from that difference.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_definitions(cslg_integration_tests PRIVATE CSLG_COMPILER_IS_GNU)
endif()
if(LOG_ELAPSED)
    target_compile_definitions(cslg_integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_ELAPSED)
endif()
if(LOG_EXCEPTIONS)
    target_compile_definitions(cslg_integration_tests PRIVATE CSLG_DEFAULT_HAS_LOG_EXCEPTIONS)
endif()

add_test(NAME cslg_integration_tests COMMAND cslg_integration_tests)
