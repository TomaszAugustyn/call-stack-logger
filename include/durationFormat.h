/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <cinttypes>
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
//   ns  <  1e12           → "[   1.000s ]" ... "[ 999.999s ]"
//   ns >=  1e12           → "[  >999.9s ]"  (saturation)
//
// Implementation uses integer math and the PRIu64 integer conversion specifier
// — deliberately avoids %f because floating-point conversion uses LC_NUMERIC,
// which would switch the decimal separator to ',' under locales like de_DE /
// fr_FR / pl_PL, producing trace files whose format drifts per-locale and
// breaks the regex-based test suite. Integer conversion is locale-independent
// by default. As a bonus this avoids all FP rounding — the displayed value
// is the true truncated integer microseconds / milliseconds / seconds.
//
// NO_INSTRUMENT: part of the instrumentation pipeline when LOG_ELAPSED=ON,
// called from __cyg_profile_func_exit; must not be instrumented itself.
NO_INSTRUMENT
inline void format_duration_12chars(std::uint64_t ns, char out[DURATION_FIELD_WIDTH + 1]) {
    // Pick unit based on range, then split into whole + 3-digit fractional part
    // using integer division. In every unit, `whole` is bounded to [0, 999]
    // (ns because input < 1000; us / ms / s because the next boundary rolls us
    // up to the next unit). So "%4" PRIu64 " always prints exactly 4 chars, and
    // "%03" PRIu64 always prints exactly 3 — yielding a fixed 12-byte output
    // without any locale or floating-point involvement.
    std::uint64_t whole = 0;
    std::uint64_t frac = 0;
    const char* unit = nullptr;

    if (ns < 1'000ULL) {
        whole = ns;
        frac = 0;
        unit = "ns";
    } else if (ns < 1'000'000ULL) {
        whole = ns / 1'000ULL;
        frac = ns % 1'000ULL;
        unit = "us";
    } else if (ns < 1'000'000'000ULL) {
        whole = ns / 1'000'000ULL;
        frac = (ns % 1'000'000ULL) / 1'000ULL;
        unit = "ms";
    } else if (ns < 1'000'000'000'000ULL) {
        // Trailing literal space after 's' so the s-unit field has the same
        // 12-char width as the other units.
        whole = ns / 1'000'000'000ULL;
        frac = (ns % 1'000'000'000ULL) / 1'000'000ULL;
        unit = "s ";
    } else {
        std::memcpy(out, DURATION_SATURATION, DURATION_FIELD_WIDTH + 1);
        return;
    }

    int written = std::snprintf(out, DURATION_FIELD_WIDTH + 1,
                                "[%4" PRIu64 ".%03" PRIu64 "%s]",
                                whole, frac, unit);

    // Defensive: if snprintf ever produces a different width, fall back to
    // saturation rather than corrupting the file. With bounded inputs this
    // branch is unreachable; the check is belt-and-suspenders.
    if (written != static_cast<int>(DURATION_FIELD_WIDTH)) {
        std::memcpy(out, DURATION_SATURATION, DURATION_FIELD_WIDTH + 1);
    }
}

} // namespace utils
