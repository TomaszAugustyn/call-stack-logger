/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <cstdlib>
#include <limits>
#include <sys/types.h> // ssize_t — POSIX type, not guaranteed by <cstdlib> (glibc leaks it, musl does not)
#include <unwind.h>

#ifndef NO_INLINE
    #define NO_INLINE __attribute__((noinline))
#endif

// NO_INSTRUMENT on everything in this header is defense-in-depth: nothing here
// may ever run instrumented (a hook firing mid-unwind would be re-entrant
// resolver code). Today that already holds indirectly — GCC excludes this
// header via the exclude-file-list, and the only instantiation lives in
// callStack.cpp, which is compiled without -finstrument-functions — but the
// attribute makes the property local instead of resting on those build-system
// invariants (e.g. a Clang user TU including this header directly would
// otherwise emit instrumented COMDAT copies the linker could pick).
#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace instrumentation {

struct Callback {
    void* caller;
    NO_INSTRUMENT Callback(void* addr) : caller(addr) {}

    NO_INSTRUMENT void operator()(void* addr) { caller = addr; }
};

template <typename F>
class FrameUnwinder {

public:
    // NO_INLINE is load-bearing: this function is frame 1 of the fixed call chain
    // behind the frame-6 constant in bfdResolver::resolve(). At -O2 (e.g. a
    // RelWithDebInfo build of the library) the optimizer otherwise inlines it into
    // its caller, shortening the chain — the unwind then overshoots the real
    // caller and every trace line gets wrong "called from" info.
    NO_INSTRUMENT NO_INLINE void unwind_nth_frame(F& f, size_t frame_number) {
        m_depth = frame_number;
        m_index = -1;
        m_pF = &f;
        // The result lands in a volatile local: a volatile write is an
        // observable side effect that must happen AFTER the call returns,
        // which keeps the call out of tail position. noinline alone is not
        // enough — at -O2 GCC otherwise turns this into `jmp _Unwind_Backtrace`
        // (sibling-call optimization, observed with GCC 16), recycling this
        // frame and removing it from the walked stack, shifting every captured
        // caller by one frame.
        volatile _Unwind_Reason_Code rc = _Unwind_Backtrace(&this->nth_frame_trampoline, this);
        (void)rc;
    }

private:
    size_t m_depth;
    ssize_t m_index;
    F* m_pF;

    NO_INSTRUMENT static _Unwind_Reason_Code nth_frame_trampoline(_Unwind_Context* ctx, void* self) {
        return (static_cast<FrameUnwinder*>(self))->nth_frame_backtrace(ctx);
    }

    NO_INSTRUMENT _Unwind_Reason_Code nth_frame_backtrace(_Unwind_Context* ctx) {
        if (m_index < (ssize_t)m_depth - 1) {
            m_index += 1;
            return _URC_NO_REASON;
        }
        return backtrace(ctx);
    }

    NO_INSTRUMENT _Unwind_Reason_Code backtrace(_Unwind_Context* ctx) {
        if (m_index >= 0 && static_cast<size_t>(m_index) >= m_depth)
            return _URC_END_OF_STACK;

        int ip_before_instruction = 0;
        uintptr_t ip = _Unwind_GetIPInfo(ctx, &ip_before_instruction);

        if (!ip_before_instruction) {
            // Calculating 0-1 for unsigned, looks like a possible bug to sanitizers.
            // so let's do it explicitly:
            if (ip == 0) {
                // Set it to 0xffff... (as from casting 0-1).
                ip = std::numeric_limits<uintptr_t>::max();
            } else {
                // Else just normally decrement it (no overflow/underflow will happen).
                ip -= 1;
            }
        }
        // Ignore first frame.
        if (m_index >= 0) {
            (*m_pF)(reinterpret_cast<void*>(ip));
        }
        m_index += 1;
        return _URC_NO_REASON;
    }
};

// Do not pass copy here, as we want to mutate `f` to get address of the n-th frame.
// NO_INLINE: frame 2 of the fixed chain behind the frame-6 constant — see the
// note on FrameUnwinder::unwind_nth_frame above.
template <typename F>
NO_INSTRUMENT NO_INLINE void unwind_nth_frame(F& f, size_t frame_number) {
    FrameUnwinder<F> unwinder;
    unwinder.unwind_nth_frame(f, frame_number);
}

} // namespace instrumentation
