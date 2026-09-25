/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Unit tests for the frame-stack reconciliation rules in include/frameReconcile.h
 * (the LOG_EXCEPTIONS repair of positional enter/exit pairing after non-local
 * exits). The rules are pure functions over a record array, so every shape the
 * hooks can meet is built here from synthetic addresses: a stack growing
 * downward from STACK_TOP, one "frame" every 0x100 bytes.
 */

#include "frameReconcile.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace {

// The record type the tests use; the tracer's own FrameRecord carries the same
// three fields (plus logging state the rules never look at).
struct Rec {
    const void* callee;
    const void* caller;
    const void* level;
};

const void* ptr(std::uintptr_t v) {
    return reinterpret_cast<const void*>(v);
}

// Synthetic code addresses (function entries and call sites).
const void* const FN_A = ptr(0x1000);
const void* const FN_B = ptr(0x2000);
const void* const FN_C = ptr(0x3000);
const void* const SITE_1 = ptr(0x1111);
const void* const SITE_2 = ptr(0x2222);
const void* const SITE_3 = ptr(0x3333);

// Synthetic stack: frame levels (canonical frame addresses) decreasing with depth.
constexpr std::uintptr_t STACK_TOP = 0x7fff'0000'0000;
const void* frame(int depth) {
    return ptr(STACK_TOP - static_cast<std::uintptr_t>(depth) * 0x100);
}

instrumentation::StackBounds whole_stack() {
    instrumentation::StackBounds b;
    b.lo = STACK_TOP - 0x10000;
    b.hi = STACK_TOP + 0x100;
    b.known = true;
    return b;
}

instrumentation::StackBounds unknown_stack() {
    return instrumentation::StackBounds{};
}

instrumentation::FrameKey key(const void* callee, const void* caller, const void* level) {
    instrumentation::FrameKey k;
    k.callee = callee;
    k.caller = caller;
    k.level = level;
    return k;
}

} // namespace

// -------- dead_records_on_enter --------

TEST(FrameReconcileTest, EnterWithNoRecordsPopsNothing) {
    std::vector<Rec> records;
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), 0, key(FN_A, SITE_1, frame(1)), whole_stack()),
              0u);
}

TEST(FrameReconcileTest, EnterOfDeeperFrameKeepsLiveAncestors) {
    // main at depth 0 calls A at depth 1: A is deeper (lower address), nothing is dead.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) } };
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), records.size(),
                                                     key(FN_B, SITE_2, frame(1)), whole_stack()),
              0u);
}

TEST(FrameReconcileTest, EnterAboveRecordsReclaimsTheDeeperOnes) {
    // A(0) -> B(1) -> C(2); C longjmps back to A, which then enters D at depth 1:
    // C(2) is below D's level (dead); B(1) has the same level as D but a
    // different caller (its slot was reused): dead too. A(0) stays.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_C, SITE_3, frame(2) } };
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), records.size(),
                                                     key(FN_C, SITE_1, frame(1)), whole_stack()),
              2u);
}

TEST(FrameReconcileTest, EnterWithEqualLevelAndEqualCallerIsAnInlineHost) {
    // B inlined into A: same level, same caller (A's return address).
    std::vector<Rec> records = { { FN_A, SITE_1, frame(1) } };
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), records.size(),
                                                     key(FN_B, SITE_1, frame(1)), whole_stack()),
              0u);
}

TEST(FrameReconcileTest, EnterFromTheSameCallSiteKeepsTheStaleRecord) {
    // A retry loop: leaf longjmps out, the loop calls it again from the SAME site
    // at the SAME level. Indistinguishable from an inlined recursive copy, so
    // the stale record is kept (reclaimed when the host exits).
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) } };
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), records.size(),
                                                     key(FN_B, SITE_2, frame(1)), whole_stack()),
              0u);
}

TEST(FrameReconcileTest, EnterScanStopsAtTheFirstRecordItKeeps) {
    // A(0) -> B(1) -> C(2); C longjmps out and the SAME call site enters B again:
    // C is deeper (dead), B is equal with the same caller (kept), so exactly one
    // record is reclaimed even though A below it would also be "not deeper".
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_C, SITE_3, frame(2) } };
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), records.size(),
                                                     key(FN_B, SITE_2, frame(1)), whole_stack()),
              1u);
}

TEST(FrameReconcileTest, EnterNeverTouchesRecordsOffTheThreadStack) {
    // A fiber frame recorded at a level outside the thread stack, then the
    // thread stack resumes at depth 1: the fiber record is not "deeper", it is
    // foreign, and the scan stops there.
    const void* fiber_frame = ptr(0x5555'0000);
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, fiber_frame } };
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), records.size(),
                                                     key(FN_C, SITE_3, frame(1)), whole_stack()),
              0u);
}

TEST(FrameReconcileTest, EnterOffTheThreadStackPopsNothing) {
    // The entering frame itself runs on a fiber stack: no level reasoning.
    const void* fiber_frame = ptr(0x5555'0000);
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(3) } };
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), records.size(),
                                                     key(FN_C, SITE_3, fiber_frame), whole_stack()),
              0u);
}

TEST(FrameReconcileTest, UnknownBoundsAssumeOneStack) {
    // pthread_getattr_np failed: every level counts as on-stack, the deeper
    // record is still reclaimed.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(2) } };
    EXPECT_EQ(instrumentation::dead_records_on_enter(records.data(), records.size(),
                                                     key(FN_C, SITE_3, frame(1)), unknown_stack()),
              1u);
}

// -------- dead_records_on_catch --------

TEST(FrameReconcileTest, CatchReclaimsEveryFrameBelowTheCatcher) {
    // catcher(1) -> mid(2) -> thrower(3); the exception is caught in catcher.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_C, SITE_3, frame(2) }, { FN_C, SITE_3, frame(3) } };
    EXPECT_EQ(instrumentation::dead_records_on_catch(records.data(), records.size(), frame(1), whole_stack()),
              2u);
}

TEST(FrameReconcileTest, CatchKeepsTheCatcherAndItsInlinedFrames) {
    // Records at the catcher's own level (the catcher and functions inlined into
    // it) are never reclaimed by level.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(1) }, { FN_B, SITE_1, frame(1) } };
    EXPECT_EQ(instrumentation::dead_records_on_catch(records.data(), records.size(), frame(1), whole_stack()),
              0u);
}

TEST(FrameReconcileTest, CatchOffTheThreadStackPopsNothing) {
    const void* fiber_frame = ptr(0x5555'0000);
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(3) } };
    EXPECT_EQ(instrumentation::dead_records_on_catch(records.data(), records.size(), fiber_frame,
                                                     whole_stack()),
              0u);
}

// -------- exiting_record_index --------

TEST(FrameReconcileTest, ExitMatchesTheTopRecordExactly) {
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, frame(1)), false, whole_stack()),
              1u);
}

TEST(FrameReconcileTest, ExitSkipsDeadRecordsAboveTheExitingFrame) {
    // B(1) -> C(2) -> C(3); the deep C frames longjmp'ed out and B returns first:
    // both C records sit above B's and are dead.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_C, SITE_3, frame(2) }, { FN_C, SITE_3, frame(3) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, frame(1)), false, whole_stack()),
              1u);
}

TEST(FrameReconcileTest, ExitWithoutAnyRecordOfTheFrameIsPositional) {
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_C, SITE_3, frame(2)), false, whole_stack()),
              1u);
}

TEST(FrameReconcileTest, ExitOfRecursionPicksTheInnermostLiveLevel) {
    // Three live levels of the same recursive call site; the innermost (top)
    // exits with its own address.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_B, SITE_2, frame(2) }, { FN_B, SITE_2, frame(3) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, frame(3)), false, whole_stack()),
              3u);
}

TEST(FrameReconcileTest, ExitOfRecursionAfterLongjmpFindsTheLevelByAddress) {
    // Levels 2 and 3 longjmp'ed back into level 1, which now returns: its exit
    // hook resolves to level 1's own level, so the exact match is index 1 and
    // the two records above it are dead.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_B, SITE_2, frame(2) }, { FN_B, SITE_2, frame(3) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, frame(1)), false, whole_stack()),
              1u);
}

TEST(FrameReconcileTest, TailCalledExitPicksTheClosestRecordBelow) {
    // void recursion at -O2: level 3's exit hook is tail-called, so it resolves
    // to level 2's level. Level 3's record is the closest one below it.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_B, SITE_2, frame(2) }, { FN_B, SITE_2, frame(3) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, frame(2)), true, whole_stack()),
              3u);
}

TEST(FrameReconcileTest, TailCalledExitAfterLongjmpSkipsDeadDeeperLevels) {
    // Levels 2 and 3 are dead (longjmp into level 1); level 1's tail-called exit
    // hook resolves to level 0's level: level 1 is the closest below, levels 2
    // and 3 are further below and therefore dead.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_B, SITE_2, frame(2) }, { FN_B, SITE_2, frame(3) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, frame(0)), true, whole_stack()),
              1u);
}

TEST(FrameReconcileTest, TailCalledExitWithNothingBelowKeepsTheTopmostMatch) {
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, frame(1)), true, whole_stack()),
              1u);
}

TEST(FrameReconcileTest, ExitWithUnresolvedLevelTakesTheTopmostMatch) {
    // Neither an exact level (the exit hook site's level could not be resolved)
    // nor a tail call: the innermost live level of the run is the frame.
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_B, SITE_2, frame(2) } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, ptr(STACK_TOP - 0x250)), false,
                                                    whole_stack()),
              2u);
}

TEST(FrameReconcileTest, ExitOnAForeignStackIsPositional) {
    // The exiting frame runs on a fiber stack: even though a matching record
    // exists further down, the tracer keeps its positional pop there.
    const void* fiber_frame = ptr(0x5555'0000);
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_C, SITE_3, fiber_frame } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, ptr(0x5555'0100)), false,
                                                    whole_stack()),
              2u);
}

TEST(FrameReconcileTest, ExitWithForeignTopRecordIsPositional) {
    // A suspended fiber's record is on top while a thread-stack frame exits: the
    // positional pop of today is kept (that scenario is wrong today as well, and
    // stays exactly as wrong, never worse).
    const void* fiber_frame = ptr(0x5555'0000);
    std::vector<Rec> records = { { FN_A, SITE_1, frame(0) }, { FN_B, SITE_2, frame(1) },
                                 { FN_C, SITE_3, fiber_frame } };
    EXPECT_EQ(instrumentation::exiting_record_index(records.data(), records.size(),
                                                    key(FN_B, SITE_2, frame(1)), false, whole_stack()),
              2u);
}

// -------- helpers --------

TEST(FrameReconcileTest, OnThreadStackHonorsTheHalfOpenRange) {
    const instrumentation::StackBounds b = whole_stack();
    EXPECT_TRUE(instrumentation::on_thread_stack(b, ptr(b.lo)));
    EXPECT_TRUE(instrumentation::on_thread_stack(b, ptr(b.hi - 1)));
    EXPECT_FALSE(instrumentation::on_thread_stack(b, ptr(b.hi)));
    EXPECT_FALSE(instrumentation::on_thread_stack(b, ptr(b.lo - 1)));
    EXPECT_TRUE(instrumentation::on_thread_stack(unknown_stack(), ptr(0x1)));
}
