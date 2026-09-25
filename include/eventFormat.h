/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include "format.h"
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

// Formatting of the exception EVENT lines LOG_EXCEPTIONS adds to the trace
// (pure, unit-tested). An event line sits in the tree like a call would, but
// ends in the "!! " glyph instead of "|_ ", and names the event instead of a
// function:
//
//   [ts] [  throw   ] |  |  |  !! throw std::runtime_error "bad header"  (thrown at: parser.cpp:77)
//   [ts] [ rethrow  ] |  |  !! rethrow std::logic_error  (rethrown at: server.cpp:61 via std::rethrow_exception)
//   [ts] [  catch   ] |  !! catch std::runtime_error "bad header"  (caught at: server.cpp:50)
//
// The bracketed column after the timestamp exists only with LOG_ELAPSED (the
// caller passes it as `after_timestamp`, like the "[  pending ] " splice of a
// call line), and the optional address column carries the event's site.

namespace utils {

// Longest what() text an event line carries, in bytes, before it is cut with a
// trailing "...". Keeps one event on one line of sane length.
inline constexpr std::size_t WHAT_TEXT_CAPACITY = 128;

// Copies a what() text into `out` (capacity `cap`, NUL-terminated) for display
// on ONE line: every control character (newlines, tabs, ...) becomes a space,
// and a text longer than cap - 1 bytes is cut and ends with "...". Never
// allocates. Returns the number of characters written, excluding the NUL.
NO_INSTRUMENT
inline std::size_t sanitize_what_into(const char* what, char* out, std::size_t cap) {
    if (cap == 0) {
        return 0;
    }
    if (what == nullptr) {
        out[0] = '\0';
        return 0;
    }
    const std::size_t length = std::strlen(what);
    const std::size_t limit = cap - 1;
    const bool cut = length > limit;
    // Leave room for the "..." when cutting (and cap is large enough for it).
    const std::size_t copied = cut ? (limit > 3 ? limit - 3 : limit) : length;
    for (std::size_t i = 0; i < copied; ++i) {
        const unsigned char c = static_cast<unsigned char>(what[i]);
        out[i] = (c < 0x20 || c == 0x7f) ? ' ' : what[i];
    }
    std::size_t size = copied;
    if (cut && limit > 3) {
        std::memcpy(out + size, "...", 3);
        size += 3;
    }
    out[size] = '\0';
    return size;
}

// One exception event to render. `site_file` / `site_line` are the resolved
// location of the throw, rethrow or catch statement, in the same form the
// resolver produces for call sites (a file, or a fallback text with no line);
// a null `site_file` renders the parenthesis with `site_label` alone (the
// terminate line: "(no handler found)").
struct EventLine {
    const char* timestamp = "";
    std::optional<void*> site_address; // set with LOG_ADDR: the event's site
    int depth = 0;                     // tree depth of the event line (>= 0)
    const char* verb = "";             // "throw", "rethrow", "catch"
    const char* type_name = "";        // demangled exception type
    const char* what = nullptr;        // already sanitized; nullptr when unknown
    const char* site_label = "";       // "thrown at", "rethrown at", "caught at"
    const std::string* site_file = nullptr;
    std::optional<unsigned int> site_line;
    const char* suffix = "";           // "" or " via std::rethrow_exception"
};

// Renders `event` into `buf` (capacity `cap`), returning the number of bytes
// written (at most `cap`, NOT NUL-terminated, like format_into). The layout is
// the call line's: "[<timestamp>] ", then `after_timestamp` verbatim (the
// LOG_ELAPSED column word plus a space, or ""), then the address column when
// `site_address` is set, then "|  " per depth level above 1 and the "!! "
// glyph, then the event text and its site. A line at depth 0 has no tree
// prefix and starts straight with "!! ". Longer lines are clamped exactly like
// call lines.
NO_INSTRUMENT
inline std::size_t format_event_into(char* buf, std::size_t cap, const EventLine& event,
                                     const char* after_timestamp = "", bool append_newline = false) {
    if (cap == 0) {
        return 0;
    }
    int pos = 0;
    int remaining = static_cast<int>(cap);

    int n = std::snprintf(buf, cap, "[%s] ", event.timestamp);
    if (n > 0 && n < remaining) {
        pos += n;
        remaining -= n;
    }
    if (after_timestamp && *after_timestamp && remaining > 0) {
        n = std::snprintf(buf + pos, remaining, "%s", after_timestamp);
        if (n > 0 && n < remaining) {
            pos += n;
            remaining -= n;
        }
    }
    if (event.site_address && remaining > 0) {
        n = std::snprintf(
                buf + pos, remaining, "addr: [0x%0*" PRIxPTR "] ",
                static_cast<int>(sizeof(void*) * 2),
                reinterpret_cast<uintptr_t>(*event.site_address));
        if (n > 0 && n < remaining) {
            pos += n;
            remaining -= n;
        }
    }
    for (int i = 1; i < event.depth && remaining > 3; ++i) {
        std::memcpy(buf + pos, "|  ", 3);
        pos += 3;
        remaining -= 3;
    }
    if (remaining > 3) {
        std::memcpy(buf + pos, "!! ", 3);
        pos += 3;
        remaining -= 3;
    }
    if (remaining > 0 && event.site_file == nullptr) {
        n = event.what != nullptr
                ? std::snprintf(buf + pos, remaining, "%s %s \"%s\"  (%s%s)", event.verb, event.type_name,
                                event.what, event.site_label, event.suffix)
                : std::snprintf(buf + pos, remaining, "%s %s  (%s%s)", event.verb, event.type_name,
                                event.site_label, event.suffix);
        if (n > 0) {
            pos += (n < remaining) ? n : remaining - 1;
        }
    } else if (remaining > 0) {
        const char* file = event.site_file->c_str();
        if (event.what != nullptr && event.site_line) {
            n = std::snprintf(buf + pos, remaining, "%s %s \"%s\"  (%s: %s:%u%s)", event.verb,
                              event.type_name, event.what, event.site_label, file, *event.site_line,
                              event.suffix);
        } else if (event.what != nullptr) {
            n = std::snprintf(buf + pos, remaining, "%s %s \"%s\"  (%s: %s:\?\?\?%s)", event.verb,
                              event.type_name, event.what, event.site_label, file, event.suffix);
        } else if (event.site_line) {
            n = std::snprintf(buf + pos, remaining, "%s %s  (%s: %s:%u%s)", event.verb, event.type_name,
                              event.site_label, file, *event.site_line, event.suffix);
        } else {
            n = std::snprintf(buf + pos, remaining, "%s %s  (%s: %s:\?\?\?%s)", event.verb,
                              event.type_name, event.site_label, file, event.suffix);
        }
        if (n > 0) {
            pos += (n < remaining) ? n : remaining - 1;
        }
    }
    if (append_newline) {
        buf[pos++] = '\n';
    }
    return static_cast<std::size_t>(pos);
}

// std::string-returning wrapper for the unit tests.
NO_INSTRUMENT
inline std::string format_event(const EventLine& event, const char* after_timestamp = "",
                                bool append_newline = false) {
    char buf[FORMAT_BUF_SIZE];
    const std::size_t size = format_event_into(buf, sizeof(buf), event, after_timestamp, append_newline);
    return std::string(buf, size);
}

} // namespace utils
