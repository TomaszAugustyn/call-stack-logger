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
