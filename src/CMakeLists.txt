#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

file(GLOB_RECURSE HEADERS "../include/*.h")

# Static library: compiled WITHOUT instrumentation flags. All functions in the library
# have NO_INSTRUMENT attribute, so instrumentation hooks are never needed here.
# exceptions.cpp holds the LOG_EXCEPTIONS interposers of the C++ runtime's
# throw/catch entry points; it compiles to nothing without that option.
add_library(callstacklogger STATIC callStack.cpp trace.cpp exceptions.cpp ${HEADERS})
cslg_apply_sanitizers(callstacklogger)
# Position-independent, so that the archive links into position-independent
# executables (the default on Debian, Ubuntu and most distributions) and into
# fixed-address ones alike, whatever the host compiler's own default is.
set_target_properties(callstacklogger PROPERTIES POSITION_INDEPENDENT_CODE ON)

# PUBLIC include dir: consumers get the include path automatically when they link
# callstacklogger — no manual target_include_directories needed in user code.
target_include_directories(callstacklogger PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)

# The symbolizer (libbacktrace, built in third_party/libbacktrace from the
# pinned sources) is PRIVATE: it reaches every executable that links
# callstacklogger through the static link, but its headers and symbols are
# invisible to consumers (hidden visibility, see that CMakeLists.txt). dl is
# for dladdr1(); Threads is needed for pthread_setcancelstate() in the hooks
# (part of libc since glibc 2.34, a separate libpthread before that).
find_package(Threads REQUIRED)
target_link_libraries(callstacklogger
    PRIVATE cslg_libbacktrace
    PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)

# 64-bit file offsets on 32-bit Linux targets: the LOG_ELAPSED cursor and
# pwrite() patch offsets are off_t, which defaults to 32 bits there, and
# per-function trace files can exceed 2 GiB easily. PRIVATE is load-bearing:
# off_t never crosses the public API, and the definition must not leak into
# consumers' TUs where it would change THEIR off_t-based ABIs.
target_compile_definitions(callstacklogger PRIVATE _FILE_OFFSET_BITS=64)

# INTERFACE compile option: -g is required for the resolver to find source
# filenames and line numbers in consumer code (they come from the DWARF debug
# information). Without it, trace output shows "???" for the "called from"
# field. Propagating means users get it automatically — compatible with any
# CMAKE_BUILD_TYPE (additive, doesn't override -O levels).
target_compile_options(callstacklogger INTERFACE -g)

# INTERFACE link options: the symbols the executable must export for dlopen()ed
# plugins and the exception interposers (see cslg_set_consumer_link_options in
# the root CMakeLists.txt). Propagating them means every executable linking
# callstacklogger gets them at link time — no user-side boilerplate.
cslg_set_consumer_link_options(callstacklogger)

# INTERFACE target that bundles everything needed to trace a user executable:
# - INSTRUMENT_FLAGS (compile-time: -finstrument-functions and, on GCC, the
#   exclude-file-list so std library headers aren't instrumented)
# - callstacklogger itself (transitively: include dir, -g, the exported hook
#   symbols, -ldl, the symbolizer)
#
# Users integrating via add_subdirectory just do:
#     target_link_libraries(my_app PRIVATE callstacklogger_instrumented)
#
# When a user wants the on-demand get_call_stack() API but NOT per-call tracing,
# they link plain `callstacklogger` instead.
#
# With DISABLE_INSTRUMENTATION=ON the target contributes NOTHING — no
# -finstrument-functions, no -g, no exported symbols, no library — so a
# release build that keeps the target in its link line is the same binary as
# one that never knew the library. A program that also calls the on-demand
# API links callstacklogger::callstacklogger explicitly for it.
add_library(callstacklogger_instrumented INTERFACE)
if(NOT DISABLE_INSTRUMENTATION)
    target_compile_options(callstacklogger_instrumented INTERFACE ${INSTRUMENT_FLAGS})
    target_link_libraries(callstacklogger_instrumented INTERFACE callstacklogger)
endif()

# Namespace aliases so users can spell the dependency as callstacklogger::…
# (signals that the target comes from an external package, not local code).
add_library(callstacklogger::callstacklogger ALIAS callstacklogger)
add_library(callstacklogger::instrumented ALIAS callstacklogger_instrumented)

# LOG_ADDR / LOG_ELAPSED gate #ifdef blocks in callStack.cpp and trace.cpp only —
# nothing in the public headers tests them. PRIVATE is load-bearing: as PUBLIC
# definitions they would reach every consumer TU that links callstacklogger,
# where an unrelated identifier named e.g. LOG_ADDR (an enum member, a function)
# would silently break under the macro.
if(LOG_ADDR)
    target_compile_definitions(callstacklogger PRIVATE LOG_ADDR)
endif()

if(LOG_ELAPSED)
    target_compile_definitions(callstacklogger PRIVATE LOG_ELAPSED)
endif()

if(LOG_EXCEPTIONS)
    target_compile_definitions(callstacklogger PRIVATE LOG_EXCEPTIONS)
endif()

# Demo executable: only built when cslg is the top-level project (standalone
# build). When a parent project pulls cslg in via add_subdirectory, runDemo is
# skipped — it's not useful and would clutter the parent's build output.
if(CSLG_TOP_LEVEL)
    add_executable(runDemo main.cpp)
    target_link_libraries(runDemo PRIVATE callstacklogger_instrumented)
    cslg_apply_sanitizers(runDemo)
endif()
