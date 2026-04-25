/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace utils {

// Width of the duration field including the surrounding brackets. The LOG_ELAPSED
// patching path pwrites exactly this many bytes on top of the placeholder. It is
// a hard invariant: every path through format_duration_12chars() MUST produce
// exactly this many characters (+ NUL terminator).
inline constexpr std::size_t DURATION_FIELD_WIDTH = 12;

// Placeholder written on enter, patched on exit. Exactly DURATION_FIELD_WIDTH chars.
// Visible on crash: un-patched lines show "[  pending ]" — a debugging feature
// that identifies which frames were still active at the crash site.
inline constexpr const char* DURATION_PLACEHOLDER = "[  pending ]";
static_assert(sizeof("[  pending ]") - 1 == DURATION_FIELD_WIDTH,
              "DURATION_PLACEHOLDER must be exactly DURATION_FIELD_WIDTH chars");

// Saturation sentinel for durations too large to fit in the 's ' range.
inline constexpr const char* DURATION_SATURATION = "[  >999.9s ]";
static_assert(sizeof("[  >999.9s ]") - 1 == DURATION_FIELD_WIDTH,
              "DURATION_SATURATION must be exactly DURATION_FIELD_WIDTH chars");

// Formats a nanosecond duration into a fixed-width 12-char field with auto-scaled
// SI units. Writes exactly DURATION_FIELD_WIDTH chars + NUL terminator into `out`.
// `out` MUST point to a buffer of at least DURATION_FIELD_WIDTH + 1 (13) bytes.
//
// Output examples:
//   ns  <  1'000          → "[   0.000ns]" ... "[ 999.000ns]"
//   ns  <  1'000'000      → "[   1.000us]" ... "[ 999.999us]"
//   ns  <  1'000'000'000  → "[   1.000ms]" ... "[ 999.999ms]"
//   ns  <  1e12           → "[   1.000s ]" ... "[1000.000s ]"  (upper edge rounds up)
//   ns >=  1e12           → "[  >999.9s ]"  (saturation)
//
// NO_INSTRUMENT: part of the instrumentation pipeline when LOG_ELAPSED=ON,
// called from __cyg_profile_func_exit; must not be instrumented itself.
NO_INSTRUMENT
inline void format_duration_12chars(std::uint64_t ns, char out[DURATION_FIELD_WIDTH + 1]) {
    // `%8.3f` is used throughout: 8 chars wide, 3 decimal digits. For our
    // bounded values (< 1000 in every unit before we up-scale), the output is
    // always 7 or 8 chars — snprintf right-aligns, giving us a fixed 8 chars.
    int written = -1;
    if (ns < 1'000ULL) {
        written = std::snprintf(out, DURATION_FIELD_WIDTH + 1,
                                "[%8.3fns]", static_cast<double>(ns));
    } else if (ns < 1'000'000ULL) {
        written = std::snprintf(out, DURATION_FIELD_WIDTH + 1,
                                "[%8.3fus]", static_cast<double>(ns) / 1'000.0);
    } else if (ns < 1'000'000'000ULL) {
        written = std::snprintf(out, DURATION_FIELD_WIDTH + 1,
                                "[%8.3fms]", static_cast<double>(ns) / 1'000'000.0);
    } else if (ns < 1'000'000'000'000ULL) {
        // Trailing literal space after 's' so the s-unit field has the same
        // 12-char width as the other units.
        written = std::snprintf(out, DURATION_FIELD_WIDTH + 1,
                                "[%8.3fs ]", static_cast<double>(ns) / 1'000'000'000.0);
    } else {
        std::memcpy(out, DURATION_SATURATION, DURATION_FIELD_WIDTH + 1);
        return;
    }

    // Defensive: if snprintf ever produces a different width (truncation,
    // locale-induced decimal separator, etc.), fall back to saturation rather
    // than corrupting the file. With a C/"POSIX" locale and bounded inputs
    // this branch is unreachable; the check is belt-and-suspenders.
    if (written != static_cast<int>(DURATION_FIELD_WIDTH)) {
        std::memcpy(out, DURATION_SATURATION, DURATION_FIELD_WIDTH + 1);
    }
}

} // namespace utils
