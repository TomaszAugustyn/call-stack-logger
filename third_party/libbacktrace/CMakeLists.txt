#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# libbacktrace (https://github.com/ianlancetaylor/libbacktrace, BSD-3-Clause)
# reads the symbol table and the DWARF debug information of the program's
# object files for the resolver in src/callStack.cpp. It replaced libbfd
# (GNU binutils, GPL-3.0-or-later): a program that ships with libbfd inside
# is a GPL program, which the commercial license cannot change. See
# THIRD_PARTY.md at the repository root.
#
# The sources are fetched from the pinned commit below at configure time and
# compiled here with hand-written config/config.h and
# config/backtrace-supported.h instead of upstream's configure script (which
# the GPL-licensed autotools helpers in the repository serve; only the C
# sources, the headers and LICENSE are used). Nothing in the fetched tree is
# modified. To build without network access, point
# FETCHCONTENT_SOURCE_DIR_LIBBACKTRACE at a checkout of that commit.
#
# Only the files the resolver needs are compiled: the ELF/DWARF reader, the
# mmap-based allocator and file views (not alloc.c / read.c), the sorting and
# atomics helpers and the state. The unwinder-based entry points (backtrace.c,
# simple.c, print.c) are not used and not compiled. elf.c is not listed
# either: src/backtraceExtension.c includes it, so that the extension can call
# elf_add(), libbacktrace's internal per-file registration (see that file).

include(FetchContent)

# 2026-09-03, "libbacktrace: make backtrace_syminfo_to_full_callback_moredata static"
set(CSLG_LIBBACKTRACE_COMMIT 0b9b49cf4a2c9229fc052d6716e1528b2f23e91a)

# Once a build directory holds the sources, reconfiguring never touches the
# network again (same policy as the Google Test download in tests/).
set(FETCHCONTENT_UPDATES_DISCONNECTED_LIBBACKTRACE ON)
FetchContent_Declare(
    libbacktrace
    GIT_REPOSITORY https://github.com/ianlancetaylor/libbacktrace.git
    GIT_TAG        ${CSLG_LIBBACKTRACE_COMMIT}
)
# Upstream has no CMakeLists.txt, so this only downloads the sources.
FetchContent_MakeAvailable(libbacktrace)

set(CSLG_LIBBACKTRACE_SOURCE_DIR "${libbacktrace_SOURCE_DIR}" CACHE INTERNAL "libbacktrace sources in use")

add_library(cslg_libbacktrace STATIC
    ${libbacktrace_SOURCE_DIR}/atomic.c
    ${libbacktrace_SOURCE_DIR}/dwarf.c
    ${libbacktrace_SOURCE_DIR}/fileline.c
    ${libbacktrace_SOURCE_DIR}/mmap.c
    ${libbacktrace_SOURCE_DIR}/mmapio.c
    ${libbacktrace_SOURCE_DIR}/posix.c
    ${libbacktrace_SOURCE_DIR}/sort.c
    ${libbacktrace_SOURCE_DIR}/state.c
    ${PROJECT_SOURCE_DIR}/src/backtraceExtension.c
)

# PUBLIC so that the resolver's own translation units find backtrace.h and the
# configuration headers; callstacklogger links this target PRIVATE, so neither
# directory reaches a consumer of the library.
target_include_directories(cslg_libbacktrace PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${libbacktrace_SOURCE_DIR})

# Hidden visibility keeps every backtrace_* symbol out of the dynamic symbol
# table of the user's executable — another libbacktrace in the same process
# (Boost.Stacktrace's, GCC's own in libgo/libphobos) must not bind to this
# copy or the other way round. Warnings are silenced: the code is upstream's,
# compiled with a configuration upstream did not generate.
set_target_properties(cslg_libbacktrace PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_compile_options(cslg_libbacktrace PRIVATE -w)
# Same 64-bit off_t as the library (see src/CMakeLists.txt): file offsets in
# the ELF reader are off_t.
target_compile_definitions(cslg_libbacktrace PRIVATE _FILE_OFFSET_BITS=64)
# Sanitized together with the library in the sanitizer builds; the flags stay
# PRIVATE to cslg targets, as everywhere else.
cslg_apply_sanitizers(cslg_libbacktrace)
