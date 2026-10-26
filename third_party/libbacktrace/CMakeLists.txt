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
# The sources are downloaded from the pinned commit below at configure time
# (FetchContent, the archive's SHA-256 checked, no git needed — the same
# mechanism as the Google Test download in tests/) and compiled here with
# hand-written config/config.h and config/backtrace-supported.h instead of
# upstream's configure script (which the GPL-licensed autotools helpers in
# the repository serve; only the C sources, the headers and LICENSE are
# used). Nothing in the downloaded tree is modified. To build without
# network access, point FETCHCONTENT_SOURCE_DIR_CSLG_LIBBACKTRACE at an
# unpacked copy of that commit; the files are checked against the pinned
# commit either way (see below).
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
set(CSLG_LIBBACKTRACE_SHA256 fabcb6868b3d09734d4c537637dacac0bf8c9aa28e07486de5c6ce2eb749d413)

# SHA-256 over the SHA-256 digests (lowercase hex, concatenated in this order)
# of the files compiled from the pinned commit; see the check below.
set(CSLG_LIBBACKTRACE_FILES
    atomic.c backtrace.h dwarf.c elf.c fileline.c internal.h mmap.c mmapio.c posix.c sort.c state.c)
set(CSLG_LIBBACKTRACE_FILES_SHA256 058e6612c093367ddffada33f25268d9589ea92a50e2d6457a99aec3b5d80f97)

# The content is named cslg_libbacktrace, not libbacktrace: FetchContent names
# are global, and the first declaration of a name wins. A parent project that
# declares its own "libbacktrace" would otherwise silently replace these
# sources (and a later dependency asking for libbacktrace would get them).
#
# Once a build directory holds the sources, reconfiguring never touches the
# network again (same policy as the Google Test download in tests/).
set(FETCHCONTENT_UPDATES_DISCONNECTED_CSLG_LIBBACKTRACE ON)
# DOWNLOAD_EXTRACT_TIMESTAMP (CMake 3.24) gives extracted files the extraction
# time as their timestamp (CMP0135 NEW behavior); older CMake does not know
# the keyword and would take it for part of URL_HASH, failing the configure.
set(CSLG_EXTRACT_TIMESTAMP_ARGS)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(CSLG_EXTRACT_TIMESTAMP_ARGS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()
FetchContent_Declare(
    cslg_libbacktrace
    URL      https://github.com/ianlancetaylor/libbacktrace/archive/${CSLG_LIBBACKTRACE_COMMIT}.tar.gz
    URL_HASH SHA256=${CSLG_LIBBACKTRACE_SHA256}
    ${CSLG_EXTRACT_TIMESTAMP_ARGS}
)
# Upstream has no CMakeLists.txt, so this only downloads and unpacks the sources.
FetchContent_MakeAvailable(cslg_libbacktrace)

# URL_HASH checks the download, but FETCHCONTENT_SOURCE_DIR_CSLG_LIBBACKTRACE
# skips the download altogether. The extension depends on libbacktrace's
# internals (elf_add(), struct backtrace_state) and config/ on the commit's
# configuration macros, so the files actually compiled must be the pinned
# commit's, whichever way they arrived.
set(CSLG_LIBBACKTRACE_DIGESTS "")
foreach(file IN LISTS CSLG_LIBBACKTRACE_FILES)
    file(SHA256 "${cslg_libbacktrace_SOURCE_DIR}/${file}" digest)
    string(APPEND CSLG_LIBBACKTRACE_DIGESTS "${digest}")
endforeach()
string(SHA256 CSLG_LIBBACKTRACE_DIGEST "${CSLG_LIBBACKTRACE_DIGESTS}")
if(NOT CSLG_LIBBACKTRACE_DIGEST STREQUAL CSLG_LIBBACKTRACE_FILES_SHA256)
    message(FATAL_ERROR "The libbacktrace sources in ${cslg_libbacktrace_SOURCE_DIR} are not those of "
            "commit ${CSLG_LIBBACKTRACE_COMMIT}, which call-stack-logger is pinned to. Unpack "
            "https://github.com/ianlancetaylor/libbacktrace/archive/${CSLG_LIBBACKTRACE_COMMIT}.tar.gz "
            "and point FETCHCONTENT_SOURCE_DIR_CSLG_LIBBACKTRACE at it, or unset that variable.")
endif()

set(CSLG_LIBBACKTRACE_SOURCE_DIR "${cslg_libbacktrace_SOURCE_DIR}"
    CACHE INTERNAL "libbacktrace sources in use")

add_library(cslg_libbacktrace STATIC
    ${cslg_libbacktrace_SOURCE_DIR}/atomic.c
    ${cslg_libbacktrace_SOURCE_DIR}/dwarf.c
    ${cslg_libbacktrace_SOURCE_DIR}/fileline.c
    ${cslg_libbacktrace_SOURCE_DIR}/mmap.c
    ${cslg_libbacktrace_SOURCE_DIR}/mmapio.c
    ${cslg_libbacktrace_SOURCE_DIR}/posix.c
    ${cslg_libbacktrace_SOURCE_DIR}/sort.c
    ${cslg_libbacktrace_SOURCE_DIR}/state.c
    ${PROJECT_SOURCE_DIR}/src/backtraceExtension.c
)

# PUBLIC so that the resolver's own translation units find backtrace.h and the
# configuration headers; callstacklogger links this target PRIVATE, so neither
# directory reaches a consumer of the library.
target_include_directories(cslg_libbacktrace PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${cslg_libbacktrace_SOURCE_DIR})

# Hidden visibility keeps every backtrace_* symbol out of the dynamic symbol
# table of the user's executable — another libbacktrace in the same process
# (Boost.Stacktrace's, GCC's own in libgo/libphobos) must not bind to this
# copy or the other way round. A static link needs more: every function of
# this copy is also renamed with a cslg_vendored_ prefix
# (config/cslg_backtrace_rename.h, included by config.h), so a program that
# links another libbacktrace statically gets no second definition of
# backtrace_create_state & co. Warnings are silenced: the code is upstream's,
# compiled with a configuration upstream did not generate. -O2 in every build
# type: the symbolizer is third-party code nobody steps through, and the
# distribution's libbfd it replaced was an optimized build whatever the
# consumer's build type — unoptimized, the first lookups in an object took
# about twice as long (bench/).
set_target_properties(cslg_libbacktrace PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_compile_options(cslg_libbacktrace PRIVATE -w -O2)
# Same 64-bit off_t as the library (see src/CMakeLists.txt): file offsets in
# the ELF reader are off_t.
target_compile_definitions(cslg_libbacktrace PRIVATE _FILE_OFFSET_BITS=64)
# Sanitized together with the library in the sanitizer builds; the flags stay
# PRIVATE to cslg targets, as everywhere else.
cslg_apply_sanitizers(cslg_libbacktrace)
