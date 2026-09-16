/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include "types.h"
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace utils {

// Capacity of the stack buffer a formatted trace line is built in. Longer lines
// are clamped (never overflow): the tail snprintf truncates and the result
// fills the buffer to capacity - 1 bytes (+ 1 for the optional newline).
inline constexpr std::size_t FORMAT_BUF_SIZE = 2048;

// Formats a resolved frame into a trace log line with timestamp, optional address,
// tree indentation, function name, and caller location, written into `buf`
// (capacity `cap`). Returns the number of bytes written — at most `cap`, and the
// output is NOT NUL-terminated (the enter hook hands the byte count to fwrite).
// This is the allocation-free core: it takes the non-owning ResolvedFrameView so
// the hook never copies the cached strings, and it writes into the caller's
// stack buffer so the line never becomes a std::string. format() below wraps it
// for callers that want a std::string.
//
// `after_timestamp` is inserted verbatim immediately after the "[<timestamp>] "
// prefix and before everything else (addr / tree / name / caller). It exists so
// LOG_ELAPSED can splice a fixed-width "[  pending ] " duration placeholder into
// every line at a position whose byte offset is independent of LOG_ADDR.
// The default empty string preserves today's output byte-for-
// byte — all existing call sites are unaffected. This header stays flag-agnostic:
// the caller in trace.cpp decides what, if anything, to splice in.
//
// `append_newline` writes the terminating '\n' into the same buffer, so the
// enter hook gets a ready-to-write line in one go.
//
// NO_INSTRUMENT: this function is part of the instrumentation pipeline (called from
// __cyg_profile_func_enter) and must not be instrumented itself.
NO_INSTRUMENT
inline std::size_t format_into(char* buf, std::size_t cap,
                               const instrumentation::ResolvedFrameView& frame,
                               int current_stack_depth, const char* after_timestamp = "",
                               bool append_newline = false) {
    if (cap == 0) {
        return 0;
    }
    int pos = 0;
    int remaining = static_cast<int>(cap);

    // Timestamp
    int n = std::snprintf(buf, cap, "[%s] ", frame.timestamp);
    if (n > 0 && n < remaining) {
        pos += n;
        remaining -= n;
    }

    // Optional splice immediately after the timestamp (LOG_ELAPSED uses this
    // to reserve the fixed-width duration field). If `after_timestamp` is the
    // default empty string this is a no-op and incurs no bytes.
    if (after_timestamp && *after_timestamp && remaining > 0) {
        n = std::snprintf(buf + pos, remaining, "%s", after_timestamp);
        if (n > 0 && n < remaining) {
            pos += n;
            remaining -= n;
        }
    }

    // Optional address (when LOG_ADDR is defined)
    if (frame.callee_address && remaining > 0) {
        n = std::snprintf(
                buf + pos, remaining, "addr: [0x%0*" PRIxPTR "] ",
                static_cast<int>(sizeof(void*) * 2),
                reinterpret_cast<uintptr_t>(*frame.callee_address));
        if (n > 0 && n < remaining) {
            pos += n;
            remaining -= n;
        }
    }

    // Tree indentation: "|  " for each depth level above 1, then "|_ " for the last level.
    // At depth 0 (top-level function like main), no indentation is added.
    for (int i = 1; i < current_stack_depth && remaining > 3; ++i) {
        std::memcpy(buf + pos, "|  ", 3);
        pos += 3;
        remaining -= 3;
    }
    if (current_stack_depth > 0 && remaining > 3) {
        std::memcpy(buf + pos, "|_ ", 3);
        pos += 3;
        remaining -= 3;
    }

    // Function name and caller location
    if (remaining > 0) {
        if (frame.caller_line_number) {
            n = std::snprintf(
                    buf + pos, remaining, "%s  (called from: %s:%u)",
                    frame.callee_function_name->c_str(),
                    frame.caller_filename->c_str(),
                    *frame.caller_line_number);
        } else {
            n = std::snprintf(
                    buf + pos, remaining, "%s  (called from: %s:\?\?\?)",
                    frame.callee_function_name->c_str(),
                    frame.caller_filename->c_str());
        }
        if (n > 0) {
            // snprintf returns the count that WOULD be written; clamp to actual space.
            pos += (n < remaining) ? n : remaining - 1;
        }
    }

    if (append_newline) {
        // Every branch above keeps pos <= cap - 1 (each write requires
        // n < remaining, and the final clamp lands at remaining - 1), so this
        // write stays in bounds. A truncated line still ends with the newline.
        buf[pos++] = '\n';
    }

    return static_cast<std::size_t>(pos);
}

// std::string-returning wrapper around format_into() for an owning
// ResolvedFrame. The unit tests exercise the line layout through this
// function; the enter hook calls format_into() directly with its own stack
// buffer so a traced call allocates nothing.
//
// NO_INSTRUMENT: this function is part of the instrumentation pipeline (called from
// __cyg_profile_func_enter) and must not be instrumented itself.
NO_INSTRUMENT
inline std::string format(const instrumentation::ResolvedFrame& frame, int current_stack_depth,
                          const char* after_timestamp = "", bool append_newline = false) {
    instrumentation::ResolvedFrameView view;
    view.timestamp = frame.timestamp.c_str();
    view.callee_address = frame.callee_address;
    view.callee_function_name = &frame.callee_function_name;
    view.caller_filename = &frame.caller_filename;
    view.caller_line_number = frame.caller_line_number;

    char buf[FORMAT_BUF_SIZE];
    const std::size_t size =
            format_into(buf, sizeof(buf), view, current_stack_depth, after_timestamp, append_newline);
    return std::string(buf, size);
}

} // namespace utils
