/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/* Gives every external function of the vendored libbacktrace a name of its
   own: backtrace_<name> becomes cslg_vendored_backtrace_<name>. Hidden
   visibility keeps this copy's symbols out of the dynamic symbol table, but
   not out of a static link: a program that links another libbacktrace
   statically — its own vendored copy, Boost.Stacktrace's libbacktrace
   backend — would define backtrace_create_state & co. twice, which fails to
   link, or binds the tracer's calls to the other copy where the linker does
   not pull this copy's definition. GCC's libsanitizer renames its copy the
   same way (backtrace-rename.h).

   Included first by config.h, which every libbacktrace source includes before
   anything else, and by src/backtraceExtension.h ahead of backtrace.h. The
   list covers the functions backtrace.h and internal.h declare, compiled here
   or not, except the backtrace_atomic_* functions: with HAVE_ATOMIC_FUNCTIONS
   (config.h) those are macros of internal.h, and no definition exists. */
#pragma once

#define backtrace_alloc cslg_vendored_backtrace_alloc
#define backtrace_close cslg_vendored_backtrace_close
#define backtrace_create_state cslg_vendored_backtrace_create_state
#define backtrace_dwarf_add cslg_vendored_backtrace_dwarf_add
#define backtrace_free cslg_vendored_backtrace_free
#define backtrace_full cslg_vendored_backtrace_full
#define backtrace_get_view cslg_vendored_backtrace_get_view
#define backtrace_initialize cslg_vendored_backtrace_initialize
#define backtrace_open cslg_vendored_backtrace_open
#define backtrace_pcinfo cslg_vendored_backtrace_pcinfo
#define backtrace_print cslg_vendored_backtrace_print
#define backtrace_qsort cslg_vendored_backtrace_qsort
#define backtrace_release_view cslg_vendored_backtrace_release_view
#define backtrace_simple cslg_vendored_backtrace_simple
#define backtrace_syminfo cslg_vendored_backtrace_syminfo
#define backtrace_syminfo_to_full_callback cslg_vendored_backtrace_syminfo_to_full_callback
#define backtrace_syminfo_to_full_error_callback cslg_vendored_backtrace_syminfo_to_full_error_callback
#define backtrace_uncompress_lzma cslg_vendored_backtrace_uncompress_lzma
#define backtrace_uncompress_zdebug cslg_vendored_backtrace_uncompress_zdebug
#define backtrace_uncompress_zstd cslg_vendored_backtrace_uncompress_zstd
#define backtrace_vector_finish cslg_vendored_backtrace_vector_finish
#define backtrace_vector_grow cslg_vendored_backtrace_vector_grow
#define backtrace_vector_release cslg_vendored_backtrace_vector_release
