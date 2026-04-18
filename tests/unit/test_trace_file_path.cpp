/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Unit tests for utils::resolve_base_trace_path() and utils::build_trace_filename()
 * in include/traceFilePath.h. Both are pure functions — no I/O, no globals — so they
 * can be tested in isolation without any program state or linked instrumentation.
 */

#include "traceFilePath.h"
#include <climits>
#include <gtest/gtest.h>

// ---------- resolve_base_trace_path ----------

TEST(ResolveBaseTracePathTest, NullReturnsDefault) {
    EXPECT_EQ(utils::resolve_base_trace_path(nullptr),
              std::string(utils::DEFAULT_TRACE_FILENAME));
}

TEST(ResolveBaseTracePathTest, EmptyStringReturnsDefault) {
    EXPECT_EQ(utils::resolve_base_trace_path(""),
              std::string(utils::DEFAULT_TRACE_FILENAME));
}

TEST(ResolveBaseTracePathTest, AbsolutePathPassedThrough) {
    EXPECT_EQ(utils::resolve_base_trace_path("/tmp/foo"), "/tmp/foo");
}

TEST(ResolveBaseTracePathTest, RelativePathPassedThrough) {
    EXPECT_EQ(utils::resolve_base_trace_path("relative/path.log"), "relative/path.log");
}

TEST(ResolveBaseTracePathTest, PathWithSpacesPassedThrough) {
    EXPECT_EQ(utils::resolve_base_trace_path("/tmp/my trace file.out"),
              "/tmp/my trace file.out");
}

TEST(ResolveBaseTracePathTest, PathWithSpecialCharsPassedThrough) {
    EXPECT_EQ(utils::resolve_base_trace_path("/var/log/trace-123.out"),
              "/var/log/trace-123.out");
}

// ---------- build_trace_filename ----------

TEST(BuildTraceFilenameTest, MainThreadReturnsBaseUnchanged) {
    EXPECT_EQ(utils::build_trace_filename("trace.out", true, 12345), "trace.out");
}

TEST(BuildTraceFilenameTest, MainThreadWithAbsolutePath) {
    EXPECT_EQ(utils::build_trace_filename("/tmp/foo.log", true, 99), "/tmp/foo.log");
}

TEST(BuildTraceFilenameTest, WorkerThreadAppendsSuffix) {
    EXPECT_EQ(utils::build_trace_filename("trace.out", false, 12345),
              "trace.out_tid_12345");
}

TEST(BuildTraceFilenameTest, WorkerThreadWithAbsolutePath) {
    EXPECT_EQ(utils::build_trace_filename("/tmp/foo.log", false, 42),
              "/tmp/foo.log_tid_42");
}

TEST(BuildTraceFilenameTest, WorkerThreadWithSmallTid) {
    EXPECT_EQ(utils::build_trace_filename("trace.out", false, 1),
              "trace.out_tid_1");
}

TEST(BuildTraceFilenameTest, WorkerThreadWithLargeTid) {
    // Very large TID must format without truncation.
    const long big_tid = LONG_MAX;
    std::string result = utils::build_trace_filename("trace.out", false, big_tid);
    EXPECT_EQ(result, std::string("trace.out_tid_") + std::to_string(big_tid));
}

TEST(BuildTraceFilenameTest, WorkerThreadSuffixAppendedToPathWithSpaces) {
    EXPECT_EQ(utils::build_trace_filename("/tmp/with spaces.log", false, 7),
              "/tmp/with spaces.log_tid_7");
}

TEST(BuildTraceFilenameTest, MainFlagIgnoresTidValue) {
    // When is_main=true, the tid parameter is ignored regardless of value.
    EXPECT_EQ(utils::build_trace_filename("trace.out", true, 0), "trace.out");
    EXPECT_EQ(utils::build_trace_filename("trace.out", true, -1), "trace.out");
    EXPECT_EQ(utils::build_trace_filename("trace.out", true, LONG_MAX), "trace.out");
}
