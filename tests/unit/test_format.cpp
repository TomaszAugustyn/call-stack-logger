/*
 * Unit tests for utils::format() in include/format.h
 *
 * Tests the trace line formatting function with various ResolvedFrame
 * configurations and nesting depths.
 */

#include "format.h"
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
