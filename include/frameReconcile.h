/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <cstddef>
#include <cstdint>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

// Frame-stack reconciliation for LOG_EXCEPTIONS (pure logic, unit-tested).
//
// The hooks keep one record per instrumented frame on a per-thread stack: every
// enter pushes, every exit pops. That pairing is positional, so it silently
// breaks whenever a frame leaves WITHOUT running its exit hook: Clang emits no
// exit hook on the exception-unwind path, and longjmp() skips the hooks of
// every frame it jumps over on both compilers. The stale record then makes the
// next exit pop the wrong record, one level off per skipped frame, for the
// rest of the thread's life (tree indentation drifts, and with LOG_ELAPSED the
// durations land on the wrong lines).
//
// The repair keys every record with the frame's LEVEL: its canonical frame
// address (CFA), the stack pointer's value just before the call that created
// the frame. Stacks grow downward, so a frame deeper than the one currently
// running always has a LOWER level, and a deeper frame cannot still be alive
// under a running shallower one. A record whose level lies below the running
// frame's therefore belongs to a frame that left without its exit hook: it is
// dead, and the tracer pops it (and marks its line) instead of letting it
// corrupt the pairing. The level is the CFA and not the hook's own frame
// address on purpose: the latter also depends on the callee's frame size, so
// two different functions called from the same place would not compare equal.
// trace.cpp derives the CFA from the hook's frame address with a per-hook-site
// offset measured once with the unwinder (see frame_level() there).
//
// The functions below only compute WHAT to pop; the hooks apply it. They are
// templates over the record type so the tracer's private FrameRecord stays
// private to trace.cpp (see the comment there) while the unit tests exercise
// every rule with a record type of their own.

namespace instrumentation {

// Identity of one instrumented frame as the hooks see it: the callee and
// caller addresses passed to __cyg_profile_func_enter/exit, plus the frame's
// level (its CFA, see above). `callee` and `caller` together identify an
// activation site (recursion repeats both, one record per level); `level`
// orders the frames.
struct FrameKey {
    const void* callee = nullptr;
    const void* caller = nullptr;
    const void* level = nullptr;
};

// Extent [lo, hi) of the current thread's own stack. Levels outside it belong
// to fibers, stackful coroutines or a sigaltstack handler: those stacks
// interleave in the one per-thread record stack today (a documented
// limitation), and comparing levels across stacks could only make that worse,
// so the rules below never touch records off the thread stack and never act
// on behalf of a frame that runs off it. `known == false` (pthread_getattr_np
// failed) treats every level as on-stack, i.e. assumes a single stack.
struct StackBounds {
    std::uintptr_t lo = 0;
    std::uintptr_t hi = 0;
    bool known = false;
};

NO_INSTRUMENT
inline std::uintptr_t stack_address(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p);
}

NO_INSTRUMENT
inline bool on_thread_stack(const StackBounds& bounds, const void* p) {
    const std::uintptr_t a = stack_address(p);
    return !bounds.known || (a >= bounds.lo && a < bounds.hi);
}

template <class Record>
NO_INSTRUMENT inline bool same_activation_site(const Record& record, const FrameKey& key) {
    return record.callee == key.callee && record.caller == key.caller;
}

// Number of records at the TOP of the stack that belong to frames no longer
// alive when a frame identified by `key` is ENTERED. Scanning down from the
// top, a record is dead when
//   * its level lies BELOW key.level: that frame was deeper than the one now
//     running and cannot still be alive, so it left without its exit hook; or
//   * its level EQUALS key.level but its caller differs: the new frame was
//     called from the same place as a frame that left without its exit hook
//     (its stack slot is being reused). Equal level with an EQUAL caller is
//     kept: an inlined copy runs inside its host's frame, so it has the host's
//     level and reads the host's return address as `caller`; that shape is a
//     live inline host (or a recursive inlined copy). A dead frame re-called
//     from the very same call site (a setjmp/longjmp retry loop) has that
//     shape too and is kept as well; it is reclaimed when its host exits (see
//     exiting_record_index), costing one level of extra indentation until
//     then, never a wrong pairing.
// The scan stops at the first record that is neither, and does nothing at all
// when the entering frame or the record examined is off the thread stack.
template <class Record>
NO_INSTRUMENT inline std::size_t dead_records_on_enter(const Record* records, std::size_t count,
                                                       const FrameKey& key, const StackBounds& bounds) {
    if (!on_thread_stack(bounds, key.level)) {
        return 0;
    }
    std::size_t dead = 0;
    while (dead < count) {
        const Record& record = records[count - 1 - dead];
        if (!on_thread_stack(bounds, record.level)) {
            break;
        }
        const bool deeper = stack_address(record.level) < stack_address(key.level);
        const bool reused_slot = record.level == key.level && record.caller != key.caller;
        if (!deeper && !reused_slot) {
            break;
        }
        ++dead;
    }
    return dead;
}

// Number of records at the top that belong to frames unwound by an exception
// that is being CAUGHT in a frame at `catch_level` — the level of the frame
// whose landing pad called the tracer's __cxa_begin_catch interposer, i.e. the
// catcher's own level as its enter hook recorded it. Every record below that
// level is a frame the exception unwound. The catcher's own record has the
// same level and is kept, as is any frame inlined into the catcher; dead
// inlined frames are told apart from live ones by the inline chain of the
// catch site, not by level.
template <class Record>
NO_INSTRUMENT inline std::size_t dead_records_on_catch(const Record* records, std::size_t count,
                                                       const void* catch_level, const StackBounds& bounds) {
    if (!on_thread_stack(bounds, catch_level)) {
        return 0;
    }
    std::size_t dead = 0;
    while (dead < count) {
        const Record& record = records[count - 1 - dead];
        if (!on_thread_stack(bounds, record.level)
            || !(stack_address(record.level) < stack_address(catch_level))) {
            break;
        }
        ++dead;
    }
    return dead;
}

// Index of the record that belongs to the frame identified by `key` when that
// frame EXITS. Every record above the returned index is dead: a frame that
// exits is the innermost live one, so anything entered after it and still
// recorded left without its exit hook. Returns count - 1 (the plain positional
// pop the tracer always did) when no record carries the frame's callee and
// caller, which keeps today's behavior for frames whose enter was never
// recorded and for frames on a foreign stack. Among the records that do carry
// them (recursion produces a contiguous run, one per level, all called from
// the same site) the frame's own record is found as follows:
//   * a normally CALLED exit hook resolves to the frame's own level, so the
//     record with the identical level is the one (only the frame itself can
//     have exactly that level, dead deeper levels are lower);
//   * a TAIL-CALLED exit hook (`jmp __cyg_profile_func_exit`, which both
//     compilers emit at -O2 for functions returning void) runs after the frame
//     was popped, so it resolves to the CALLER's level and the frame's own
//     record is the closest one BELOW it: dead deeper levels are further
//     below, live outer levels are at or above it;
//   * when neither applies (the level could not be resolved for that hook
//     site, see frame_level() in trace.cpp), the topmost record of the run is
//     the frame, as the innermost live level.
// `count` must be non-zero.
template <class Record>
NO_INSTRUMENT inline std::size_t exiting_record_index(const Record* records, std::size_t count,
                                                      const FrameKey& key, bool tail_called,
                                                      const StackBounds& bounds) {
    const std::size_t top = count - 1;
    if (!on_thread_stack(bounds, key.level) || !on_thread_stack(bounds, records[top].level)) {
        return top;
    }
    std::size_t first = top;
    while (!same_activation_site(records[first], key)) {
        if (first == 0) {
            return top;
        }
        --first;
    }
    std::size_t best = first;
    if (tail_called) {
        bool found_below = false;
        for (std::size_t i = first;; --i) {
            const Record& record = records[i];
            if (!same_activation_site(record, key)) {
                break;
            }
            const bool below = stack_address(record.level) < stack_address(key.level);
            if (below && (!found_below || stack_address(record.level) > stack_address(records[best].level))) {
                best = i;
                found_below = true;
            }
            if (i == 0) {
                break;
            }
        }
    } else {
        for (std::size_t i = first;; --i) {
            const Record& record = records[i];
            if (!same_activation_site(record, key)) {
                break;
            }
            if (record.level == key.level) {
                best = i;
                break;
            }
            if (i == 0) {
                break;
            }
        }
    }
    return best;
}

} // namespace instrumentation
