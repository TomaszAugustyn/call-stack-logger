/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Unit tests for utils::format() in include/format.h
 *
 * Tests the trace line formatting function with various ResolvedFrame
 * configurations and nesting depths.
 */

#include "format.h"
#include "prettyTime.h"
#include "types.h"
#include <gtest/gtest.h>
#include <string>

namespace {

// Helper to create a ResolvedFrame with common defaults.
instrumentation::ResolvedFrame make_frame(
        const std::string& func_name,
        const std::string& caller_file = "test.cpp",
        std::optional<unsigned int> caller_line = 42,
        std::optional<void*> address = std::nullopt) {
    instrumentation::ResolvedFrame frame;
    frame.timestamp = "01-01-2025 12:00:00.000";
    frame.callee_function_name = func_name;
    frame.caller_filename = caller_file;
    frame.caller_line_number = caller_line;
    frame.callee_address = address;
    return frame;
}

} // namespace

// Depth 0: top-level function, no tree indentation.
TEST(FormatTest, DepthZeroNoIndentation) {
    auto frame = make_frame("main");
    std::string result = utils::format(frame, 0);

    EXPECT_NE(result.find("[01-01-2025 12:00:00.000]"), std::string::npos);
    EXPECT_NE(result.find("main"), std::string::npos);
    EXPECT_NE(result.find("(called from: test.cpp:42)"), std::string::npos);
    // No tree prefix at depth 0
    EXPECT_EQ(result.find("|_ "), std::string::npos);
    EXPECT_EQ(result.find("|  "), std::string::npos);
}

// Depth 1: single |_ prefix.
TEST(FormatTest, DepthOnePrefix) {
    auto frame = make_frame("A::foo()");
    std::string result = utils::format(frame, 1);

    EXPECT_NE(result.find("|_ A::foo()"), std::string::npos);
    // Should not have continuation prefix
    EXPECT_EQ(result.find("|  "), std::string::npos);
}

// Depth 3: two continuation prefixes + one branch prefix.
TEST(FormatTest, DepthThreeNestedIndentation) {
    auto frame = make_frame("deep_func");
    std::string result = utils::format(frame, 3);

    EXPECT_NE(result.find("|  |  |_ deep_func"), std::string::npos);
}

// Depth 5: four continuation prefixes + one branch prefix.
TEST(FormatTest, DepthFiveDeeplyNested) {
    auto frame = make_frame("leaf");
    std::string result = utils::format(frame, 5);

    EXPECT_NE(result.find("|  |  |  |  |_ leaf"), std::string::npos);
}

// Line number absent: shows ??? instead.
TEST(FormatTest, MissingLineNumberShowsQuestionMarks) {
    auto frame = make_frame("unknown_loc", "lib.so", std::nullopt);
    std::string result = utils::format(frame, 1);

    EXPECT_NE(result.find("(called from: lib.so:\?\?\?)"), std::string::npos);
}

// Address present: addr: [0x...] appears before tree prefix.
TEST(FormatTest, AddressPresentWhenSet) {
    void* test_addr = reinterpret_cast<void*>(0xDEADBEEF);
    auto frame = make_frame("with_addr", "test.cpp", 10, test_addr);
    std::string result = utils::format(frame, 1);

    EXPECT_NE(result.find("addr: [0x"), std::string::npos);
    EXPECT_NE(result.find("deadbeef"), std::string::npos);
}

// Address absent: no addr: prefix.
TEST(FormatTest, NoAddressWhenNotSet) {
    auto frame = make_frame("no_addr");
    std::string result = utils::format(frame, 1);

    EXPECT_EQ(result.find("addr:"), std::string::npos);
}

// Timestamp appears in brackets.
TEST(FormatTest, TimestampInBrackets) {
    auto frame = make_frame("func");
    std::string result = utils::format(frame, 0);

    EXPECT_EQ(result.substr(0, 1), "[");
    EXPECT_NE(result.find("] "), std::string::npos);
}

// Long function name: should not crash, output is truncated gracefully.
TEST(FormatTest, LongFunctionNameHandled) {
    std::string long_name(1500, 'X');
    auto frame = make_frame(long_name);
    std::string result = utils::format(frame, 1);

    // Should produce non-empty output without crashing
    EXPECT_FALSE(result.empty());
    // The output should contain at least the timestamp
    EXPECT_NE(result.find("[01-01-2025"), std::string::npos);
}

// Empty function name: valid edge case.
TEST(FormatTest, EmptyFunctionName) {
    auto frame = make_frame("");
    std::string result = utils::format(frame, 0);

    EXPECT_NE(result.find("(called from: test.cpp:42)"), std::string::npos);
}

// Guards the LOG_ELAPSED byte-offset invariant. trace.cpp computes the
// placeholder's absolute file offset as `cursor + utils::PRETTY_TIME_LENGTH + 3`,
// which assumes `format()` emits the `after_timestamp` argument immediately after
// the "[<timestamp>] " prefix (= 1 + PRETTY_TIME_LENGTH + 2 bytes). If format()
// ever re-orders the prefix — e.g., by putting the address column before the
// splice — this test fails before LOG_ELAPSED ships a corrupted trace file.
TEST(FormatTest, AfterTimestampLandsAtExpectedOffset) {
    auto frame = make_frame("placeholder_offset_probe");
    std::string result = utils::format(frame, 1, "[  pending ] ");

    constexpr std::size_t EXPECTED_OFFSET = utils::PRETTY_TIME_LENGTH + 3;
    ASSERT_GE(result.size(), EXPECTED_OFFSET + 12)
            << "Output too short to contain the placeholder.";
    EXPECT_EQ(result.substr(EXPECTED_OFFSET, 12), "[  pending ]")
            << "Placeholder is not at byte offset " << EXPECTED_OFFSET
            << ". LOG_ELAPSED's pwrite offset derivation (PRETTY_TIME_LENGTH + 3) "
               "has drifted from what format() produces. Full line was:\n"
            << result;
    EXPECT_EQ(result.substr(0, EXPECTED_OFFSET), "[01-01-2025 12:00:00.000] ")
            << "The 26-byte prefix is not exactly \"[<timestamp>] \" as the "
               "offset math assumes.";
}

// Same guard combined with LOG_ADDR's address column: the address must land
// AFTER the after_timestamp splice, not before. trace.cpp relies on this
// ordering so the placeholder offset stays independent of LOG_ADDR.
TEST(FormatTest, AfterTimestampPrecedesAddressColumn) {
    void* addr = reinterpret_cast<void*>(0xDEADBEEF);
    auto frame = make_frame("func_with_addr", "f.cpp", 10, addr);
    std::string result = utils::format(frame, 1, "[  pending ] ");

    constexpr std::size_t EXPECTED_OFFSET = utils::PRETTY_TIME_LENGTH + 3;
    EXPECT_EQ(result.substr(EXPECTED_OFFSET, 12), "[  pending ]");
    // addr must come after the placeholder, not before.
    const auto addr_pos = result.find("addr:");
    ASSERT_NE(addr_pos, std::string::npos);
    EXPECT_GT(addr_pos, EXPECTED_OFFSET + 12)
            << "addr: column appears at offset " << addr_pos
            << " — should be AFTER the placeholder (which ends at "
            << (EXPECTED_OFFSET + 12) << "). LOG_ELAPSED layout has regressed.";
}
