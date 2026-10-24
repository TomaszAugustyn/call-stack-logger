/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/* Hand-written configuration for libbacktrace on Linux with glibc (or musl),
   replacing the config.h its configure script would generate. The macros are
   the ones the compiled sources test (atomic.c, dwarf.c, fileline.c, elf.c,
   mmap.c, mmapio.c, posix.c, state.c); the others in upstream's config.h.in
   are for Windows, macOS, AIX, the BSDs and the test suite.

   HAVE_SYNC_FUNCTIONS is load-bearing: without it backtrace_create_state()
   refuses to create a THREADED state, and the resolver always creates one. */
#pragma once

#define HAVE_ATOMIC_FUNCTIONS 1
#define HAVE_SYNC_FUNCTIONS 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DECL_GETPAGESIZE 1
#define HAVE_DECL_STRNLEN 1
#define HAVE_DLFCN_H 1
#define HAVE_DL_ITERATE_PHDR 1
#define HAVE_FCNTL 1
#define HAVE_GETIPINFO 1
#define HAVE_INTTYPES_H 1
#define HAVE_LINK_H 1
#define HAVE_LSTAT 1
#define HAVE_READLINK 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

/* dl_iterate_phdr, pread and strnlen are GNU/POSIX extensions. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* The ELF class follows the pointer size of the build, like configure's
   check of __SIZEOF_POINTER__ / the size of void*. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#define BACKTRACE_ELF_SIZE 64
#else
#define BACKTRACE_ELF_SIZE 32
#endif
