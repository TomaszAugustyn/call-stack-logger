/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Unit tests for the exception event line formatter in include/eventFormat.h
 * (LOG_EXCEPTIONS: the "!! throw ..." / "!! rethrow ..." / "!! catch ..."
 * lines) and the what() sanitizer that keeps such a line on one line.
 */

#include "eventFormat.h"
#include <cstring>
#include <gtest/gtest.h>
#include <string>

namespace {

const std::string kFile = "/src/parser.cpp";

utils::EventLine make_throw() {
    utils::EventLine e;
    e.timestamp = "01-01-2025 12:00:00.000";
    e.depth = 3;
    e.verb = "throw";
    e.type_name = "std::runtime_error";
    e.what = "bad header";
    e.site_label = "thrown at";
    e.site_file = &kFile;
    e.site_line = 77;
    return e;
}

} // namespace

// -------- sanitize_what_into --------

TEST(EventFormatTest, SanitizeCopiesPlainText) {
    char out[32];
    EXPECT_EQ(utils::sanitize_what_into("bad header", out, sizeof(out)), 10u);
    EXPECT_STREQ(out, "bad header");
}

TEST(EventFormatTest, SanitizeReplacesControlCharactersWithSpaces) {
    char out[32];
    utils::sanitize_what_into("line one\nline two\ttabbed\x01\x7f", out, sizeof(out));
    EXPECT_STREQ(out, "line one line two tabbed  ");
}

TEST(EventFormatTest, SanitizeCutsLongTextWithEllipsis) {
    const std::string text(300, 'x');
    char out[utils::WHAT_TEXT_CAPACITY];
    const std::size_t n = utils::sanitize_what_into(text.c_str(), out, sizeof(out));
    EXPECT_EQ(n, utils::WHAT_TEXT_CAPACITY - 1);
    EXPECT_EQ(std::strlen(out), utils::WHAT_TEXT_CAPACITY - 1);
    EXPECT_EQ(std::string(out).substr(utils::WHAT_TEXT_CAPACITY - 4), "...");
    EXPECT_EQ(std::string(out).substr(0, 10), "xxxxxxxxxx");
}

TEST(EventFormatTest, SanitizeHandlesNullAndTinyBuffers) {
    char out[4] = { 'z', 'z', 'z', 'z' };
    EXPECT_EQ(utils::sanitize_what_into(nullptr, out, sizeof(out)), 0u);
    EXPECT_STREQ(out, "");
    EXPECT_EQ(utils::sanitize_what_into("abcdef", out, sizeof(out)), 3u);
    EXPECT_STREQ(out, "abc"); // no room for "..." in a 4-byte buffer: plain cut
    EXPECT_EQ(utils::sanitize_what_into("abc", out, 0), 0u);
}

// -------- format_event_into --------

TEST(EventFormatTest, ThrowLineHasTreePrefixGlyphTypeWhatAndSite) {
    const std::string line = utils::format_event(make_throw());
    EXPECT_EQ(line,
              "[01-01-2025 12:00:00.000] |  |  !! throw std::runtime_error \"bad header\"  "
              "(thrown at: /src/parser.cpp:77)");
}

TEST(EventFormatTest, DepthZeroAndOneHaveNoInnerPrefix) {
    auto e = make_throw();
    e.depth = 0;
    EXPECT_EQ(utils::format_event(e).substr(26, 3), "!! ");
    e.depth = 1;
    EXPECT_EQ(utils::format_event(e).substr(26, 3), "!! ");
    e.depth = 2;
    EXPECT_EQ(utils::format_event(e).substr(26, 6), "|  !! ");
}

TEST(EventFormatTest, CatchLineWithoutWhatOmitsTheQuotes) {
    auto e = make_throw();
    e.verb = "catch";
    e.what = nullptr;
    e.site_label = "caught at";
    e.site_line = 50;
    e.depth = 1;
    EXPECT_EQ(utils::format_event(e),
              "[01-01-2025 12:00:00.000] !! catch std::runtime_error  (caught at: /src/parser.cpp:50)");
}

TEST(EventFormatTest, RethrowLineCarriesTheSuffix) {
    auto e = make_throw();
    e.verb = "rethrow";
    e.what = nullptr;
    e.site_label = "rethrown at";
    e.suffix = " via std::rethrow_exception";
    e.depth = 2;
    EXPECT_EQ(utils::format_event(e),
              "[01-01-2025 12:00:00.000] |  !! rethrow std::runtime_error  "
              "(rethrown at: /src/parser.cpp:77 via std::rethrow_exception)");
}

TEST(EventFormatTest, UnknownLineShowsQuestionMarks) {
    auto e = make_throw();
    e.site_line.reset();
    const std::string fallback = "std::__throw_out_of_range_fmt(char const*, ...)";
    e.site_file = &fallback;
    EXPECT_NE(utils::format_event(e).find("(thrown at: std::__throw_out_of_range_fmt(char const*, ...):\?\?\?)"),
              std::string::npos);
    e.what = nullptr;
    EXPECT_NE(utils::format_event(e).find("std::runtime_error  (thrown at: std::__throw_out_of_range_fmt"),
              std::string::npos);
}

TEST(EventFormatTest, TerminateLineHasNoSite) {
    auto e = make_throw();
    e.verb = "terminate";
    e.site_label = "no handler found";
    e.site_file = nullptr;
    e.site_line.reset();
    e.depth = 2;
    EXPECT_EQ(utils::format_event(e),
              "[01-01-2025 12:00:00.000] |  !! terminate std::runtime_error \"bad header\"  (no handler found)");
    e.what = nullptr;
    EXPECT_EQ(utils::format_event(e),
              "[01-01-2025 12:00:00.000] |  !! terminate std::runtime_error  (no handler found)");
}

// The LOG_ELAPSED column word goes right after the timestamp, and with LOG_ADDR
// the site address follows it — the same order as on call lines, so the tree
// glyph offset math (utils::tree_glyph_offset) applies to event lines too.
TEST(EventFormatTest, ColumnsFollowTheCallLineOrder) {
    auto e = make_throw();
    e.site_address = reinterpret_cast<void*>(0x4006f2);
    e.depth = 2;
    const std::string line = utils::format_event(e, "[  throw   ] ");
    EXPECT_EQ(line.substr(0, 26), "[01-01-2025 12:00:00.000] ");
    EXPECT_EQ(line.substr(26, 13), "[  throw   ] ");
    EXPECT_EQ(line.substr(39, 9), "addr: [0x");
    EXPECT_EQ(line.substr(39 + utils::ADDR_COLUMN_WIDTH, 6), "|  !! ");
    EXPECT_EQ(line.find("!! "), utils::tree_glyph_offset(2, 13 + utils::ADDR_COLUMN_WIDTH));
}

TEST(EventFormatTest, NewlineAndClampingBehaveLikeCallLines) {
    auto e = make_throw();
    const std::string with_nl = utils::format_event(e, "", true);
    EXPECT_EQ(with_nl.back(), '\n');
    EXPECT_EQ(with_nl.substr(0, with_nl.size() - 1), utils::format_event(e));

    const std::string huge(3000, 'T');
    e.type_name = huge.c_str();
    char buf[utils::FORMAT_BUF_SIZE];
    const std::size_t size = utils::format_event_into(buf, sizeof(buf), e, "", true);
    EXPECT_EQ(size, utils::FORMAT_BUF_SIZE);
    EXPECT_EQ(buf[size - 1], '\n');
}
