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

// Boundary: 999'999'999 ns → ms unit (rounds up); 1'000'000'000 ns → s unit.
TEST(DurationFormatTest, MsToSBoundary) {
    // 999'999'999 ns = 999.999999 ms, rounds to 1000.000 with %.3f — still
    // 8 chars, still fits the field width. This is the documented edge case.
    EXPECT_EQ(format_and_check(999'999'999ULL), "[1000.000ms]");
    EXPECT_EQ(format_and_check(1'000'000'000ULL), "[   1.000s ]");
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
