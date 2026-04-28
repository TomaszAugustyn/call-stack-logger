/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Unit tests for utils::format_duration_12chars() in include/durationFormat.h
 *
 * The duration field is a HARD 12-byte invariant — LOG_ELAPSED pwrites exactly
 * 12 bytes onto the placeholder at runtime. These tests enforce that every
 * code path produces the correct fixed width and framing characters.
 */

#include "durationFormat.h"
#include <clocale>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <string>

namespace {

// Helper: format the value and return the result as a std::string (size 12).
// Also validates all invariants in-place.
std::string format_and_check(std::uint64_t ns) {
    char buf[utils::DURATION_FIELD_WIDTH + 1] = {};
    utils::format_duration_12chars(ns, buf);

    // Every code path MUST produce exactly 12 characters (plus NUL).
    EXPECT_EQ(std::strlen(buf), utils::DURATION_FIELD_WIDTH);

    // Framing characters.
    EXPECT_EQ(buf[0], '[');
    EXPECT_EQ(buf[utils::DURATION_FIELD_WIDTH - 1], ']');

    return std::string(buf);
}

} // namespace

// -------- Framing / invariant tests --------

TEST(DurationFormatTest, WidthConstantIsTwelve) {
    EXPECT_EQ(utils::DURATION_FIELD_WIDTH, 12u);
}

TEST(DurationFormatTest, PlaceholderHasCorrectWidth) {
    EXPECT_EQ(std::strlen(utils::DURATION_PLACEHOLDER), utils::DURATION_FIELD_WIDTH);
    EXPECT_STREQ(utils::DURATION_PLACEHOLDER, "[  pending ]");
}

TEST(DurationFormatTest, SaturationSentinelHasCorrectWidth) {
    EXPECT_EQ(std::strlen(utils::DURATION_SATURATION), utils::DURATION_FIELD_WIDTH);
    EXPECT_STREQ(utils::DURATION_SATURATION, "[  >999.9s ]");
}

// -------- Nanoseconds range --------

TEST(DurationFormatTest, Zero) {
    EXPECT_EQ(format_and_check(0), "[   0.000ns]");
}

TEST(DurationFormatTest, OneNanosecond) {
    EXPECT_EQ(format_and_check(1), "[   1.000ns]");
}

TEST(DurationFormatTest, HundredsOfNanoseconds) {
    EXPECT_EQ(format_and_check(123), "[ 123.000ns]");
}

TEST(DurationFormatTest, MaxNanoseconds) {
    EXPECT_EQ(format_and_check(999), "[ 999.000ns]");
}

// -------- Microseconds range --------

TEST(DurationFormatTest, OneMicrosecond) {
    EXPECT_EQ(format_and_check(1'000ULL), "[   1.000us]");
}

TEST(DurationFormatTest, FractionalMicroseconds) {
    // 123'456 ns = 123.456 us
    EXPECT_EQ(format_and_check(123'456ULL), "[ 123.456us]");
}

TEST(DurationFormatTest, MaxMicroseconds) {
    // 999'999 ns = 999.999 us
    EXPECT_EQ(format_and_check(999'999ULL), "[ 999.999us]");
}

// -------- Milliseconds range --------

TEST(DurationFormatTest, OneMillisecond) {
    EXPECT_EQ(format_and_check(1'000'000ULL), "[   1.000ms]");
}

TEST(DurationFormatTest, FractionalMilliseconds) {
    // 123'456'000 ns = 123.456 ms
    EXPECT_EQ(format_and_check(123'456'000ULL), "[ 123.456ms]");
}

TEST(DurationFormatTest, MaxMilliseconds) {
    // 999'999'000 ns = 999.999 ms
    EXPECT_EQ(format_and_check(999'999'000ULL), "[ 999.999ms]");
}

// -------- Seconds range (trailing space in unit) --------

TEST(DurationFormatTest, OneSecond) {
    EXPECT_EQ(format_and_check(1'000'000'000ULL), "[   1.000s ]");
}

TEST(DurationFormatTest, FractionalSeconds) {
    // 123'456'000'000 ns = 123.456 s
    EXPECT_EQ(format_and_check(123'456'000'000ULL), "[ 123.456s ]");
}

TEST(DurationFormatTest, MaxSeconds) {
    // 999'999'000'000 ns = 999.999 s
    EXPECT_EQ(format_and_check(999'999'000'000ULL), "[ 999.999s ]");
}

// -------- Saturation --------

TEST(DurationFormatTest, SaturatesAtThousandSeconds) {
    // Exactly 1000 s → saturation.
    EXPECT_EQ(format_and_check(1'000'000'000'000ULL), "[  >999.9s ]");
}

TEST(DurationFormatTest, SaturatesWellBeyondThousandSeconds) {
    // 1 hour = 3600 s → well into saturation.
    EXPECT_EQ(format_and_check(3'600'000'000'000ULL), "[  >999.9s ]");
}

TEST(DurationFormatTest, SaturatesAtMaxUint64) {
    // ~584 years — must not overflow to garbage.
    EXPECT_EQ(format_and_check(std::numeric_limits<std::uint64_t>::max()),
              "[  >999.9s ]");
}

// -------- Boundary transitions --------

// Boundary: 999 ns → ns unit; 1'000 ns → us unit.
TEST(DurationFormatTest, NsToUsBoundary) {
    EXPECT_EQ(format_and_check(999ULL),   "[ 999.000ns]");
    EXPECT_EQ(format_and_check(1'000ULL), "[   1.000us]");
}

// Boundary: 999'999 ns → us unit; 1'000'000 ns → ms unit.
TEST(DurationFormatTest, UsToMsBoundary) {
    EXPECT_EQ(format_and_check(999'999ULL),   "[ 999.999us]");
    EXPECT_EQ(format_and_check(1'000'000ULL), "[   1.000ms]");
}

// Boundary: 999'999'999 ns → ms unit (truncated); 1'000'000'000 ns → s unit.
TEST(DurationFormatTest, MsToSBoundary) {
    // 999'999'999 ns. Integer math truncates at the 3-digit fraction, so
    // whole = 999, frac = 999'999 / 1'000 = 999, giving "[ 999.999ms]".
    // Nicer than the old FP behavior which rounded to "[1000.000ms]" and
    // visually suggested we'd slipped into 4-digit ms territory.
    EXPECT_EQ(format_and_check(999'999'999ULL), "[ 999.999ms]");
    EXPECT_EQ(format_and_check(1'000'000'000ULL), "[   1.000s ]");
}

// -------- Locale stress: decimal separator must not drift under LC_NUMERIC --------

// If the formatter ever uses %f / %g it would inherit LC_NUMERIC and emit ','
// instead of '.' in locales like de_DE / pl_PL / fr_FR — trace files would
// become locale-dependent and break every downstream parser. This test switches
// the C locale to one that uses ',' (when available on the host) and confirms
// the output still has a literal '.' character. If the formatter ever regresses
// to %f this test fails loudly.
TEST(DurationFormatTest, DecimalSeparatorIsLocaleIndependent) {
    // Save and restore the current LC_NUMERIC so the test is hermetic regardless
    // of what the CI image's default is.
    const char* const saved = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved_copy = saved ? saved : "C";

    const char* const comma_locales[] = {
        "de_DE.UTF-8", "pl_PL.UTF-8", "fr_FR.UTF-8", "de_DE", "pl_PL", "fr_FR",
    };
    bool switched = false;
    for (const char* loc : comma_locales) {
        if (std::setlocale(LC_NUMERIC, loc) != nullptr) {
            switched = true;
            break;
        }
    }
    if (!switched) {
        GTEST_SKIP() << "No comma-decimal locale available on this host — skipping "
                        "locale stress. (The Docker CI image ships the C locale only.) "
                        "The integer-based formatter is locale-independent by "
                        "construction, so this test is an auxiliary guard.";
    }

    char buf[utils::DURATION_FIELD_WIDTH + 1] = {};
    utils::format_duration_12chars(123'456ULL, buf);  // 123.456 us

    // Expect the dot — NOT a comma — regardless of LC_NUMERIC.
    EXPECT_NE(std::strchr(buf, '.'), nullptr)
            << "Duration field is missing '.': " << buf
            << ". The formatter may have regressed to locale-dependent %f.";
    EXPECT_EQ(std::strchr(buf, ','), nullptr)
            << "Duration field contains ',': " << buf
            << ". Decimal separator must be locale-independent.";
    EXPECT_STREQ(buf, "[ 123.456us]");

    std::setlocale(LC_NUMERIC, saved_copy.c_str());
}

// -------- Buffer untouched past DURATION_FIELD_WIDTH + NUL --------

// Verifies format_duration_12chars() does not scribble past its 13-byte window.
TEST(DurationFormatTest, DoesNotOverflowBuffer) {
    char buf[utils::DURATION_FIELD_WIDTH + 4] = {};
    // Fill with a canary byte.
    std::memset(buf, 0xAB, sizeof(buf));

    utils::format_duration_12chars(42'000'000ULL, buf);

    EXPECT_STREQ(buf, "[  42.000ms]");
    // The byte at index 12 is the terminating NUL written by snprintf.
    EXPECT_EQ(static_cast<unsigned char>(buf[utils::DURATION_FIELD_WIDTH]), 0u);
    // Subsequent bytes must still hold the canary.
    for (std::size_t i = utils::DURATION_FIELD_WIDTH + 1; i < sizeof(buf); ++i) {
        EXPECT_EQ(static_cast<unsigned char>(buf[i]), 0xABu) << "Buffer overrun at index " << i;
    }
}
