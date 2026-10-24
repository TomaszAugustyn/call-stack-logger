/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/* Hand-written counterpart of libbacktrace's generated backtrace-supported.h
   for the configuration in config.h: ELF on Linux, mmap-based allocation (no
   malloc inside the library), threads supported. */
#pragma once

#define BACKTRACE_SUPPORTED 1
#define BACKTRACE_USES_MALLOC 0
#define BACKTRACE_SUPPORTS_THREADS 1
#define BACKTRACE_SUPPORTS_DATA 1
#define BACKTRACE_SUPPORTS_MOREDATA 1
