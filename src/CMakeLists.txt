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
add_library(callstacklogger STATIC callStack.cpp trace.cpp ${HEADERS})
cslg_apply_sanitizers(callstacklogger)

# PUBLIC include dir: consumers get the include path automatically when they link
# callstacklogger — no manual target_include_directories needed in user code.
target_include_directories(callstacklogger PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)

target_link_libraries(callstacklogger PUBLIC -ldl -lbfd)

# INTERFACE compile option: -g is required for BFD to resolve source filenames
# and line numbers in consumer code. Without it, trace output shows "???" for
# the "called from" field. Propagating means users get it automatically —
# compatible with any CMAKE_BUILD_TYPE (additive, doesn't override -O levels).
target_compile_options(callstacklogger INTERFACE -g)

# INTERFACE link option: -rdynamic is required by callstacklogger's own dladdr()
# calls to resolve user symbols. Propagating it means every executable linking
# callstacklogger gets -rdynamic at link time — no user-side boilerplate.
target_link_options(callstacklogger INTERFACE -rdynamic)

# INTERFACE target that bundles everything needed to trace a user executable:
# - INSTRUMENT_FLAGS (compile-time: -finstrument-functions and, on GCC, the
#   exclude-file-list so std library headers aren't instrumented)
# - callstacklogger itself (transitively: include dir, -rdynamic, -ldl, -lbfd)
#
# Users integrating via add_subdirectory just do:
#     target_link_libraries(my_app PRIVATE callstacklogger_instrumented)
#
# When a user wants the on-demand get_call_stack() API but NOT per-call tracing,
# they link plain `callstacklogger` instead.
add_library(callstacklogger_instrumented INTERFACE)
target_compile_options(callstacklogger_instrumented INTERFACE ${INSTRUMENT_FLAGS})
target_link_libraries(callstacklogger_instrumented INTERFACE callstacklogger)

# Namespace aliases so users can spell the dependency as callstacklogger::…
# (signals that the target comes from an external package, not local code).
add_library(callstacklogger::callstacklogger ALIAS callstacklogger)
add_library(callstacklogger::instrumented ALIAS callstacklogger_instrumented)

# LOG_ADDR / LOG_NOT_DEMANGLED apply to the library itself (they gate #ifdef in
# format.h / callStack.cpp). Put them on the library target rather than globally.
if(LOG_ADDR)
    target_compile_definitions(callstacklogger PUBLIC LOG_ADDR)
endif()

if(LOG_NOT_DEMANGLED)
    target_compile_definitions(callstacklogger PUBLIC LOG_NOT_DEMANGLED)
endif()

if(LOG_ELAPSED)
    target_compile_definitions(callstacklogger PUBLIC LOG_ELAPSED)
endif()

# Demo executable: only built when cslg is the top-level project (standalone
# build). When a parent project pulls cslg in via add_subdirectory, runDemo is
# skipped — it's not useful and would clutter the parent's build output.
if(CSLG_TOP_LEVEL)
    add_executable(runDemo main.cpp)
    target_link_libraries(runDemo PRIVATE callstacklogger_instrumented)
    cslg_apply_sanitizers(runDemo)
endif()
