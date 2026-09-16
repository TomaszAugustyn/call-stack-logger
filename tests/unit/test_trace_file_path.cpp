/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Unit tests for utils::resolve_base_trace_path(), utils::make_absolute_trace_path()
 * and utils::build_trace_filename() in include/traceFilePath.h. All three are pure
 * functions — no I/O, no globals — so they can be tested in isolation without any
 * program state or linked instrumentation.
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

// ---------- make_absolute_trace_path ----------

TEST(MakeAbsoluteTracePathTest, RelativePathIsPrefixedWithCwd) {
    EXPECT_EQ(utils::make_absolute_trace_path("trace.out", "/home/user/proj"),
              "/home/user/proj/trace.out");
}

TEST(MakeAbsoluteTracePathTest, NestedRelativePathIsPrefixedWithCwd) {
    EXPECT_EQ(utils::make_absolute_trace_path("logs/run.out", "/work"), "/work/logs/run.out");
}

TEST(MakeAbsoluteTracePathTest, AbsolutePathIsUnchanged) {
    EXPECT_EQ(utils::make_absolute_trace_path("/tmp/trace.out", "/home/user"), "/tmp/trace.out");
}

TEST(MakeAbsoluteTracePathTest, NullCwdKeepsRelativePath) {
    // getcwd() failed in trace_begin(): the path stays relative and tracing continues.
    EXPECT_EQ(utils::make_absolute_trace_path("trace.out", nullptr), "trace.out");
}

TEST(MakeAbsoluteTracePathTest, EmptyCwdKeepsRelativePath) {
    EXPECT_EQ(utils::make_absolute_trace_path("trace.out", ""), "trace.out");
}

TEST(MakeAbsoluteTracePathTest, RootCwdDoesNotDoubleSeparator) {
    EXPECT_EQ(utils::make_absolute_trace_path("trace.out", "/"), "/trace.out");
}

TEST(MakeAbsoluteTracePathTest, CwdWithTrailingSlashDoesNotDoubleSeparator) {
    EXPECT_EQ(utils::make_absolute_trace_path("trace.out", "/work/"), "/work/trace.out");
}

TEST(MakeAbsoluteTracePathTest, DotRelativePathIsPrefixedVerbatim) {
    // No normalization: "./" and "../" components are left for the kernel to resolve.
    EXPECT_EQ(utils::make_absolute_trace_path("./trace.out", "/work"), "/work/./trace.out");
    EXPECT_EQ(utils::make_absolute_trace_path("../trace.out", "/work/sub"),
              "/work/sub/../trace.out");
}

TEST(MakeAbsoluteTracePathTest, EmptyPathIsUnchanged) {
    EXPECT_EQ(utils::make_absolute_trace_path("", "/work"), "");
}

TEST(MakeAbsoluteTracePathTest, DefaultNameResolvesUnderCwd) {
    // The documented default: "trace.out" relative to the directory at program start.
    const std::string base = utils::resolve_base_trace_path(nullptr);
    EXPECT_EQ(utils::make_absolute_trace_path(base, "/start/dir"), "/start/dir/trace.out");
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
