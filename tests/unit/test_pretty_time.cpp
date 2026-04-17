/*
 * Unit tests for utils::pretty_time() and utils::to_ms() in include/prettyTime.h
 *
 * Tests timestamp formatting, pattern correctness, and millisecond conversion.
 */

#include "prettyTime.h"
#include <chrono>
#include <gtest/gtest.h>
#include <regex>
#include <string>

// pretty_time() returns a string matching "DD-MM-YYYY HH:MM:SS.mmm" format.
TEST(PrettyTimeTest, MatchesExpectedFormat) {
    std::string ts = utils::pretty_time();

    // Pattern: DD-MM-YYYY HH:MM:SS.mmm
    std::regex pattern(R"(\d{2}-\d{2}-\d{4} \d{2}:\d{2}:\d{2}\.\d{3})");
    EXPECT_TRUE(std::regex_match(ts, pattern))
            << "Timestamp '" << ts << "' does not match DD-MM-YYYY HH:MM:SS.mmm format";
}

// String length should be exactly 23 characters (DD-MM-YYYY HH:MM:SS.mmm).
TEST(PrettyTimeTest, CorrectLength) {
    std::string ts = utils::pretty_time();
    EXPECT_EQ(ts.length(), 23u) << "Timestamp '" << ts << "' has unexpected length";
}

// Calling pretty_time() twice in quick succession should produce similar timestamps.
TEST(PrettyTimeTest, ConsecutiveCallsProduceSimilarTimestamps) {
    std::string ts1 = utils::pretty_time();
    std::string ts2 = utils::pretty_time();

    // The date and hour:minute portion should be identical (same second at worst)
    // Compare first 17 characters: "DD-MM-YYYY HH:MM:"
    EXPECT_EQ(ts1.substr(0, 17), ts2.substr(0, 17));
}

// Millisecond portion is 3 digits after the dot.
TEST(PrettyTimeTest, MillisecondPortion) {
    std::string ts = utils::pretty_time();

    // Find the dot separator
    auto dot_pos = ts.rfind('.');
    ASSERT_NE(dot_pos, std::string::npos) << "No dot found in timestamp";
    std::string ms_str = ts.substr(dot_pos + 1);
    EXPECT_EQ(ms_str.length(), 3u) << "Millisecond portion '" << ms_str << "' is not 3 digits";

    // All characters should be digits
    for (char c : ms_str) {
        EXPECT_TRUE(std::isdigit(c)) << "Non-digit character in milliseconds: " << c;
    }
}

// to_ms() correctly converts a known time_point to milliseconds.
TEST(ToMsTest, KnownTimePoint) {
    using namespace std::chrono;

    auto epoch = system_clock::time_point{};
    EXPECT_EQ(utils::to_ms(epoch), 0L);

    auto one_second = system_clock::time_point{seconds(1)};
    EXPECT_EQ(utils::to_ms(one_second), 1000L);

    auto half_second = system_clock::time_point{milliseconds(500)};
    EXPECT_EQ(utils::to_ms(half_second), 500L);
}

// to_ms() with steady_clock time_point.
TEST(ToMsTest, SteadyClock) {
    using namespace std::chrono;

    auto tp = steady_clock::time_point{seconds(42)};
    EXPECT_EQ(utils::to_ms(tp), 42000L);
}
