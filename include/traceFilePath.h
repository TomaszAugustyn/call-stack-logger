/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <string>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace utils {

// Fallback name used when CSLG_OUTPUT_FILE is unset or empty.
constexpr const char* DEFAULT_TRACE_FILENAME = "trace.out";

// Resolve the base trace path from the CSLG_OUTPUT_FILE environment value.
// Returns the env value if non-null and non-empty, else DEFAULT_TRACE_FILENAME.
// Pure: no globals, no I/O — safe for unit tests.
NO_INSTRUMENT
inline std::string resolve_base_trace_path(const char* env_value) {
    if (env_value == nullptr || env_value[0] == '\0') {
        return std::string(DEFAULT_TRACE_FILENAME);
    }
    return std::string(env_value);
}

// Anchor a relative trace path to a fixed directory. Returns `path` unchanged
// when it is already absolute or when `cwd` is null/empty (getcwd() failed —
// the caller keeps the relative path and continues). Otherwise returns
// "<cwd>/<path>", without doubling the separator when cwd already ends in '/'
// (the root directory). The per-thread trace files must all land in the SAME
// directory even if the program chdir()s between the main thread's lazy open
// and a worker's, so trace.cpp resolves the base path once, at startup.
// Pure: no syscalls — the caller passes in the cwd string.
NO_INSTRUMENT
inline std::string make_absolute_trace_path(const std::string& path, const char* cwd) {
    if (path.empty() || path[0] == '/' || cwd == nullptr || cwd[0] == '\0') {
        return path;
    }
    std::string result(cwd);
    if (result.back() != '/') {
        result += '/';
    }
    result += path;
    return result;
}

// Build the per-thread trace filename.
// - Main thread (is_main=true): returns base unchanged.
// - Worker thread: returns "<base>_tid_<tid>".
// Pure: no syscalls, no globals — safe for unit tests with arbitrary tid values.
NO_INSTRUMENT
inline std::string build_trace_filename(const std::string& base, bool is_main, long tid) {
    if (is_main) {
        return base;
    }
    return base + "_tid_" + std::to_string(tid);
}

} // namespace utils
