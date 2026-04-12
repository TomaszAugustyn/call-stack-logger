/*
 * Copyright © 2020-2023 Tomasz Augustyn
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

namespace utils {

// Formats a ResolvedFrame into a trace log line with timestamp, optional address,
// tree indentation, function name, and caller location.
// Uses snprintf into a stack buffer instead of std::ostringstream to avoid heap
// allocation on every traced function call.
inline std::string format(const instrumentation::ResolvedFrame& frame, int current_stack_depth) {

    constexpr size_t BUF_SIZE = 2048;
    char buf[BUF_SIZE];
    int pos = 0;
    int remaining = static_cast<int>(BUF_SIZE);

    // Timestamp
    int n = std::snprintf(buf, BUF_SIZE, "[%s] ", frame.timestamp.c_str());
    if (n > 0 && n < remaining) {
        pos += n;
        remaining -= n;
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
                    frame.callee_function_name.c_str(),
                    frame.caller_filename.c_str(),
                    *frame.caller_line_number);
        } else {
            n = std::snprintf(
                    buf + pos, remaining, "%s  (called from: %s:???)",
                    frame.callee_function_name.c_str(),
                    frame.caller_filename.c_str());
        }
        if (n > 0) {
            // snprintf returns the count that WOULD be written; clamp to actual space.
            pos += (n < remaining) ? n : remaining - 1;
        }
    }

    return std::string(buf, static_cast<size_t>(pos));
}

} // namespace utils
