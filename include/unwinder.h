#pragma once

#include <cstdlib>
#include <limits>
#include <unwind.h>

namespace instrumentation {

struct Callback {
    void* caller;
    Callback(void* addr) : caller(addr) {}

    void operator()(void* addr) { caller = addr; }
};

template <typename F>
class FrameUnwinder {

public:
    void unwind_nth_frame(F& f, size_t frame_number) {
        m_depth = frame_number;
        m_index = -1;
        m_pF = &f;
        _Unwind_Backtrace(&this->nth_frame_trampoline, this);
    }

private:
    size_t m_depth;
    ssize_t m_index;
    F* m_pF;

    static _Unwind_Reason_Code nth_frame_trampoline(_Unwind_Context* ctx, void* self) {
        return (static_cast<FrameUnwinder*>(self))->nth_frame_backtrace(ctx);
    }

    _Unwind_Reason_Code nth_frame_backtrace(_Unwind_Context* ctx) {
        if (m_index < (ssize_t)m_depth - 1) {
            m_index += 1;
            return _URC_NO_REASON;
        }
        return backtrace(ctx);
    }

    _Unwind_Reason_Code backtrace(_Unwind_Context* ctx) {
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
template <typename F>
void unwind_nth_frame(F& f, size_t frame_number) {
    FrameUnwinder<F> unwinder;
    unwinder.unwind_nth_frame(f, frame_number);
}

} // namespace instrumentation
