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

# PUBLIC include dir: consumers get the include path automatically when they link
# callstacklogger — no manual target_include_directories needed in user code.
target_include_directories(callstacklogger PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)

# CSLG_BFD_LINK_LIBS is "dl bfd" plus, when the configure-time probe in the
# root CMakeLists.txt detected that static libbfd needs it, "zstd". Threads is
# needed for pthread_setcancelstate() in the hooks (part of libc since glibc
# 2.34, a separate libpthread before that).
find_package(Threads REQUIRED)
target_link_libraries(callstacklogger PUBLIC ${CSLG_BFD_LINK_LIBS} Threads::Threads)

# 64-bit file offsets on 32-bit Linux targets: the LOG_ELAPSED cursor and
# pwrite() patch offsets are off_t, which defaults to 32 bits there, and
# per-function trace files can exceed 2 GiB easily. PRIVATE is load-bearing:
# off_t never crosses the public API, and the definition must not leak into
# consumers' TUs where it would change THEIR off_t-based ABIs.
target_compile_definitions(callstacklogger PRIVATE _FILE_OFFSET_BITS=64)

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
    # Clang emits DWARF 5 by default. libbfd (binutils 2.46 at the time of
    # writing) does not walk the DW_TAG_inlined_subroutine chain of Clang's
    # DWARF 5 output: bfd_find_inliner_info() reports no inlined functions at
    # an address inside an inlined body, although elfutils and llvm-symbolizer
    # read the same chain, and libbfd itself reads GCC's DWARF 5 chains and
    # Clang's DWARF 4 chains fine. LOG_EXCEPTIONS uses the chain at a catch
    # site (and at the entry of an inlined function) to tell frames that were
    # inlined into the catcher and unwound — dead — from the catcher's own live
    # inline chain, which share one physical frame and therefore one level.
    # Without it, Clang at -O2 marks such frames only when the inlined catcher
    # returns, and the catch line and the calls after it sit deeper meanwhile.
    # Asking Clang for DWARF 4 restores the exact behavior. This changes only
    # the debug-info format of the consumer's own objects, not code generation;
    # DWARF 4 and 5 objects mix freely in one link, and every debugger reads
    # DWARF 4. A consumer that passes -gdwarf-5 after our flag gets the
    # self-healing behavior described above. Added only with LOG_EXCEPTIONS=ON
    # under Clang; GCC keeps its default, and so does a plain Clang build, whose
    # reconciliation (on in every build) then reclaims a dead inlined frame when
    # its host returns instead of at once.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(callstacklogger INTERFACE -gdwarf-4)
    endif()
endif()

# Demo executable: only built when cslg is the top-level project (standalone
# build). When a parent project pulls cslg in via add_subdirectory, runDemo is
# skipped — it's not useful and would clutter the parent's build output.
if(CSLG_TOP_LEVEL)
    add_executable(runDemo main.cpp)
    target_link_libraries(runDemo PRIVATE callstacklogger_instrumented)
    cslg_apply_sanitizers(runDemo)
endif()
