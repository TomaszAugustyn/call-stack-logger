#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

cmake_minimum_required(VERSION 3.14)

project(call-stack-logger)

option(BUILD_TESTS "Build unit and integration tests (top-level builds only)" OFF)
option(COVERAGE "Enable code coverage (requires GCC)" OFF)

# Output-content flags (consumed in src/CMakeLists.txt as PRIVATE compile
# definitions on the library) and the instrumentation kill-switch. Declared
# here as first-class options so they show up in ccmake / cmake-gui alongside
# the rest, matching the README's CMake-options table — behavior with plain
# -D<flag>=ON on the command line is unchanged.
option(LOG_ADDR "Include callee addresses in trace output" OFF)
option(DISABLE_INSTRUMENTATION "Compile without any instrumentation hooks" OFF)

# Opt-in per-function duration tracking. When ON, every instrumented function's
# exit patches the in-place "[  pending ]" placeholder on its enter line with the
# actual elapsed duration (auto-scaled SI units), using a second non-O_APPEND fd
# and pwrite(). When OFF (default), zero runtime or code-size cost — all the
# LOG_ELAPSED machinery is inside #ifdef LOG_ELAPSED blocks. See the
# "Per-function timing" section in README.md for details.
option(LOG_ELAPSED "Record and display per-function duration in trace output" OFF)

# Opt-in exception awareness. When ON, every throw, rethrow, catch and terminate
# becomes a line in the tree and the lines of frames that an exception or a
# longjmp() left are marked in place. (The tree itself stays exact after such
# exits in every build: the hooks key frame records by stack level and reclaim
# the records of frames that left without their exit hook — see "Keeping the
# tree exact after non-local exits" in README.md.) When OFF (default), the event and
# marking code is not compiled: it is inside #ifdef LOG_EXCEPTIONS blocks.
option(LOG_EXCEPTIONS "Trace exception events and mark the frames they left in the call tree" OFF)

# LOG_EXCEPTIONS interposes the C++ runtime's throw/catch entry points: the
# tracer defines __cxa_throw & co. in the executable and forwards every call to
# the runtime's own implementation, found with dlsym(RTLD_NEXT). With
# -static-libstdc++ (or -static) the runtime is inside the executable, so there
# is nothing to forward to; the program then stops at startup with a FATAL
# message (trace_begin's verify_interposers). Only the global flags are visible
# here — a per-target link option cannot be checked — so this is a courtesy,
# the startup check is the real guard.
if(LOG_EXCEPTIONS)
    set(CSLG_ALL_LINK_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS}")
    if(CSLG_ALL_LINK_FLAGS MATCHES "-static-libstdc\\+\\+" OR CSLG_ALL_LINK_FLAGS MATCHES "(^|[ \t])-static([ \t]|$)")
        message(WARNING "LOG_EXCEPTIONS needs the shared libstdc++. The tracer defines its own "
                "__cxa_throw and forwards every throw to the C++ runtime's implementation found with "
                "RTLD_NEXT; with -static-libstdc++ (or -static) that implementation is inside the "
                "executable, so there is nothing to forward to and the program stops at startup with "
                "a FATAL message. Link the shared runtime (the default) or configure without "
                "LOG_EXCEPTIONS.")
    endif()
endif()

# Sanitizers: opt-in, for debugging / CI runs. Flags are applied PRIVATE to every
# cslg target (library + demo + tests) so external users who link callstacklogger
# never inherit sanitizer flags — they link a clean library regardless.
#
# Accepted values:
#   ""                    — no sanitizer (default)
#   "address"             — AddressSanitizer (ASan) + LeakSanitizer
#   "undefined"           — UndefinedBehaviorSanitizer (UBSan)
#   "address+undefined"   — ASan and UBSan combined (they compose)
#   "thread"              — ThreadSanitizer (TSan) — mutually exclusive with ASan
#
# MSan intentionally not supported: it needs an MSan-instrumented libstdc++/libc
# to avoid false positives across std::string/std::vector etc.
set(SANITIZE "" CACHE STRING "Sanitizer: address | undefined | address+undefined | thread")
set_property(CACHE SANITIZE PROPERTY STRINGS "" "address" "undefined" "address+undefined" "thread")

set(CSLG_SANITIZER_FLAGS "")
if(SANITIZE)
    if(SANITIZE STREQUAL "address")
        list(APPEND CSLG_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    elseif(SANITIZE STREQUAL "undefined")
        list(APPEND CSLG_SANITIZER_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined)
    elseif(SANITIZE STREQUAL "address+undefined")
        list(APPEND CSLG_SANITIZER_FLAGS -fsanitize=address -fsanitize=undefined
             -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    elseif(SANITIZE STREQUAL "thread")
        list(APPEND CSLG_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR "Unknown SANITIZE value: ${SANITIZE}. "
                "Valid: address | undefined | address+undefined | thread")
    endif()
    message(STATUS "Sanitizers enabled: ${SANITIZE} (${CSLG_SANITIZER_FLAGS})")
endif()

# Helper: apply sanitizer flags to a target as PRIVATE compile + link options.
# PRIVATE is load-bearing — INTERFACE would propagate to external users who
# link callstacklogger, which we deliberately do NOT want.
function(cslg_apply_sanitizers target)
    if(CSLG_SANITIZER_FLAGS)
        target_compile_options(${target} PRIVATE ${CSLG_SANITIZER_FLAGS})
        target_link_options(${target} PRIVATE ${CSLG_SANITIZER_FLAGS})
    endif()
endfunction()

# True when this project is built standalone (not via add_subdirectory/FetchContent
# from a parent project). Guards the demo executable, the `run` target and the
# test tree.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(CSLG_TOP_LEVEL TRUE)
else()
    set(CSLG_TOP_LEVEL FALSE)
endif()

# ============================================================================
# Compiler flags and instrumentation setup (project-wide)
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Base flags — no instrumentation (applied to all targets including library).
# Append rather than overwrite: users may pass -DCMAKE_CXX_FLAGS=... (e.g. -O2)
# on the command line, and overwriting would silently discard it. No duplication
# across reconfigures: this sets a normal variable that does not persist — every
# configure re-reads the cache value and appends once.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall -Wextra")

# Note: -rdynamic is attached to the callstacklogger target as an INTERFACE link
# option, so any executable that links against callstacklogger automatically gets
# it. This lets users integrate via add_subdirectory without per-target boilerplate.

# Code coverage support (GCC --coverage flag).
# -fprofile-update=atomic makes gcov counter updates thread-safe. Without it the
# multi-threaded cslg_threaded_traced_test_program runs the instrumented library
# (trace.cpp) on several worker threads that race on the default non-atomic
# counters; a lost update can drive a counter negative, which lcov 2.x's geninfo
# rejects with a fatal "Unexpected negative count" error — a nondeterministic
# coverage-job failure. Atomic updates remove the race at the source rather than
# relying on scheduling luck.
#
# Appending to CMAKE_CXX_FLAGS is sufficient for EVERY target in the tree:
# CMake passes the language flags to the compiler driver on the link line too,
# so --coverage reaches both compilation and linking without any per-target
# target_compile_options/target_link_options in the test CMake files.
if(COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-update=atomic")
    # The C sources too: src/backtraceExtension.c is the library's own code
    # (the open wrapper, the layout check). libbacktrace's own sources get the
    # flags with it; the coverage report drops them with the rest of _deps/.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -fprofile-update=atomic")
endif()

# Split compilation: the callstacklogger library is compiled WITHOUT instrumentation
# (all its functions have NO_INSTRUMENT attribute). INSTRUMENT_FLAGS is applied only to
# user code targets (runDemo, cslg_traced_test_program) via target_compile_options.
#
# GCC: -finstrument-functions with exclude-file-list for std library headers.
# Clang: plain -finstrument-functions. Clang lacks the exclude-file-list, so std library
#   functions are filtered at runtime via is_std_library_symbol() in callStack.cpp.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Discover C++ standard library header paths to exclude from instrumentation.
    execute_process(
        COMMAND bash -c "${CMAKE_CXX_COMPILER} -xc++ -E -v - < /dev/null 2>&1 \
        | LC_ALL=C sed -ne '/starts here/,/End of/p' \
        | grep -o '/[^\"]*'"
        COMMAND bash -c "sort"
        COMMAND bash -c "uniq"
        COMMAND bash -c "tr '\n' ','"
        COMMAND bash -c "sed 's/\\(.*\\),/\\1 /'"
        COMMAND bash -c "xargs"
        OUTPUT_VARIABLE CPP_STD_INCLUDES
    )
    string(STRIP "${CPP_STD_INCLUDES}" CPP_STD_INCLUDES)

    # The project's own instrumentation headers, excluded from instrumentation
    # alongside the std library paths discovered above.
    set(CSLG_EXCLUDED_HEADERS
        callStack.h
        durationFormat.h
        format.h
        prettyTime.h
        stdSymbolFilter.h
        traceFilePath.h
        types.h
    )

    # Absolute paths, deliberately: GCC matches exclude-file-list entries as
    # SUBSTRINGS of the compiled file's path. Relative entries like
    # "include/format.h" or "include/types.h" would also match a consumer
    # project's own headers with those common names (when cslg is pulled in via
    # add_subdirectory / FetchContent), silently de-instrumenting them. Anchoring
    # to this project's source dir matches only cslg's own headers.
    list(TRANSFORM CSLG_EXCLUDED_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/include/")
    list(JOIN CSLG_EXCLUDED_HEADERS "," CSLG_EXCLUDED_HEADERS_JOINED)

    # Plain (non-cache) variable, deliberately: the execute_process above re-creates
    # a normal CPP_STD_INCLUDES on every configure, which would shadow a cached
    # value — set(... CACHE) would then be a no-op on reconfigure and the header
    # suffixes appended here would silently vanish from INSTRUMENT_FLAGS. A plain
    # variable keeps the full list (std paths + project headers) freshly computed
    # each configure, and follows compiler upgrades in an existing build dir.
    set(CPP_STD_INCLUDES "${CPP_STD_INCLUDES},${CSLG_EXCLUDED_HEADERS_JOINED}")
    message(STATUS "CPP_STD_INCLUDES is [${CPP_STD_INCLUDES}]")

    set(INSTRUMENT_FLAGS -finstrument-functions
        -finstrument-functions-exclude-file-list=${CPP_STD_INCLUDES})

elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(INSTRUMENT_FLAGS -finstrument-functions)

else()
    message(WARNING "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Function instrumentation may not work.")
    set(INSTRUMENT_FLAGS "")
endif()

# Disable instrumentation: adds the preprocessor define and clears INSTRUMENT_FLAGS
# so that no user code target receives -finstrument-functions.
# Must be set before add_subdirectory so child scopes inherit the correct values.
if(DISABLE_INSTRUMENTATION)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDISABLE_INSTRUMENTATION")
    set(INSTRUMENT_FLAGS "")
endif()

# ============================================================================
# Symbols the user's executable must export
# ============================================================================
# Plugins loaded with dlopen() do not link the library: their instrumented
# code binds __cyg_profile_func_enter / __cyg_profile_func_exit at load time
# to the HOST executable's dynamic symbols, so the executable has to export
# the two hooks or every traced plugin fails to load. With LOG_EXCEPTIONS the
# executable also defines the C++ runtime's throw/catch entry points
# (__cxa_throw & co., src/exceptions.cpp) and must export them, or the dynamic
# linker never routes the runtime's own calls through them. Nothing else in
# the library depends on exported symbols: the resolver names functions from
# the symbol table and the debug information of the object files, not from
# the dynamic symbol table.
#
# -rdynamic (the historical choice) exports EVERY global symbol of the
# executable, which bloats its dynamic symbol table and lets shared libraries
# bind to the executable's copies of their own symbols. The narrow form,
# --export-dynamic-symbol=<name> per symbol (GNU ld since binutils 2.35, lld,
# gold), exports the names above and leaves the rest of the program's symbols
# out — except, as in any program, definitions that override a symbol of a
# shared library on the link line: the five interposers override libstdc++'s,
# so the linkers export them even without their entries. The narrow form is
# used when the linker accepts it; -rdynamic stays the fallback for older
# linkers and the explicit choice behind CSLG_EXPORT_ALL_SYMBOLS. The one observable difference: an executable
# stripped of its symbol table (strip --strip-all, no separate debug file)
# keeps its function names in the dynamic symbol table only with -rdynamic;
# with the narrow form the resolver finds no names and the trace stays empty.
option(CSLG_EXPORT_ALL_SYMBOLS
    "Link programs that trace with -rdynamic (every symbol exported) instead of exporting only the hooks"
    OFF)
# The names exported in the narrow form, as the linker sees them (mangled): the
# two hooks, and with LOG_EXCEPTIONS the five interposed entry points (see
# verify_interposers in src/exceptions.cpp) — __cxa_throw, __cxa_rethrow,
# __cxa_begin_catch, std::rethrow_exception(std::exception_ptr) and
# std::terminate(). A misspelled name exports nothing and no linker complains;
# ExportListTest checks that the library defines every one of them.
set(CSLG_HOOK_SYMBOLS __cyg_profile_func_enter __cyg_profile_func_exit)
set(CSLG_INTERPOSED_SYMBOLS __cxa_throw __cxa_rethrow __cxa_begin_catch
    _ZSt17rethrow_exceptionNSt15__exception_ptr13exception_ptrE _ZSt9terminatev)
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LINK_OPTIONS "-Wl,--export-dynamic-symbol=main")
check_cxx_source_compiles("int main() { return 0; }" CSLG_LINKER_HAS_EXPORT_DYNAMIC_SYMBOL)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

# Attaches, as INTERFACE link options of `target` (a library every tracing
# program links), the exports described above. Used for the shipped library in
# src/ and for the per-flag library variants the integration tests build.
# With DISABLE_INSTRUMENTATION there are no hooks and no interposers to export,
# so nothing is attached.
function(cslg_set_consumer_link_options target)
    if(DISABLE_INSTRUMENTATION)
        return()
    endif()
    if(CSLG_EXPORT_ALL_SYMBOLS OR NOT CSLG_LINKER_HAS_EXPORT_DYNAMIC_SYMBOL)
        target_link_options(${target} INTERFACE -rdynamic)
        return()
    endif()
    set(symbols ${CSLG_HOOK_SYMBOLS})
    if(LOG_EXCEPTIONS)
        list(APPEND symbols ${CSLG_INTERPOSED_SYMBOLS})
    endif()
    foreach(symbol IN LISTS symbols)
        target_link_options(${target} INTERFACE "-Wl,--export-dynamic-symbol=${symbol}")
    endforeach()
endfunction()

# ============================================================================
# Keeping names and lines through a later strip
# ============================================================================
# The resolver reads names from an object file's symbol table and lines from
# its DWARF data. A binary stripped with `strip --strip-all` (what
# `cmake --install --strip` and a plain `strip` do) has neither left, and its
# traced calls can no longer be named. The usual remedy is the distributions'
# own: move the debug information to a separate file before stripping and
# leave a .gnu_debuglink to it in the binary; the resolver follows the link
# (same directory, .debug/, /usr/lib/debug, or the build ID). The link
# survives a later `strip --strip-all`, only the debug sections and the symbol
# table go.
#
# cslg_keep_debug_info(<target> [INSTALL_DESTINATION <dir>]) does that for an
# executable (or shared library) target after each link: it writes
# <target-file>.debug with `objcopy --only-keep-debug` and adds the link to the
# target file with `objcopy --add-gnu-debuglink`. The binary itself is not
# stripped; whoever strips it later keeps a complete trace as long as the
# .debug file ships next to it (INSTALL_DESTINATION installs it there) or is
# installed under /usr/lib/debug. A library cannot attach a post-build step to
# a consumer's targets through an INTERFACE target, so the call is explicit.
#
# Costs to know about: the .debug file is as large as the binary's debug
# information; and once the link exists, the resolver reads the debug file
# (and checks its CRC over its whole length) instead of the binary's own
# sections, a one-time cost per object file at its first traced call.
function(cslg_keep_debug_info target)
    cmake_parse_arguments(CSLG_KDI "" "INSTALL_DESTINATION" "" ${ARGN})
    if(NOT TARGET ${target})
        message(FATAL_ERROR "cslg_keep_debug_info: ${target} is not a target")
    endif()
    # CMAKE_OBJCOPY comes from the toolchain detection; llvm-objcopy does both
    # operations as well. Without either, warn and keep the build going: the
    # program still traces, it only loses the names once it is stripped.
    set(objcopy "${CMAKE_OBJCOPY}")
    if(NOT objcopy)
        find_program(objcopy NAMES objcopy llvm-objcopy)
    endif()
    if(NOT objcopy)
        message(WARNING "cslg_keep_debug_info(${target}): no objcopy or llvm-objcopy found; "
                "no .debug file will be produced")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${objcopy} --only-keep-debug $<TARGET_FILE:${target}> $<TARGET_FILE:${target}>.debug
        COMMAND ${objcopy} --add-gnu-debuglink=$<TARGET_FILE_NAME:${target}>.debug $<TARGET_FILE:${target}>
        WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>
        COMMENT "Writing the debug information of ${target} to $<TARGET_FILE_NAME:${target}>.debug and linking it"
        VERBATIM)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.15)
        set_property(TARGET ${target} APPEND PROPERTY ADDITIONAL_CLEAN_FILES $<TARGET_FILE:${target}>.debug)
    endif()
    if(CSLG_KDI_INSTALL_DESTINATION)
        install(FILES $<TARGET_FILE:${target}>.debug DESTINATION ${CSLG_KDI_INSTALL_DESTINATION})
    endif()
endfunction()

# ============================================================================
# Targets
# ============================================================================
# The vendored symbolizer first: src/ links the target it defines.
add_subdirectory(third_party/libbacktrace)
add_subdirectory(src)

if(CSLG_TOP_LEVEL)
    add_custom_target(run COMMAND runDemo)
endif()

# Tests are built only for a standalone build. BUILD_TESTS is a common option
# name, so a parent project that pulls cslg in via add_subdirectory/FetchContent
# and has its own BUILD_TESTS=ON would otherwise get cslg's test tree (and the
# Google Test download) as a side effect. All cslg test targets and ctest names
# also carry a cslg_ prefix, so they cannot clash with a parent's own targets.
if(BUILD_TESTS AND CSLG_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif()

# The cost benchmark (bench/): a `bench` target that builds and runs it.
# Standalone builds against any checkout go through bench/run_bench.sh.
option(BUILD_BENCH "Build the cost benchmark (top-level builds only)" OFF)
if(BUILD_BENCH AND CSLG_TOP_LEVEL)
    add_subdirectory(bench)
endif()
