/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <optional>
#include <string>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace instrumentation {

struct ResolvedFrame {
    std::string timestamp;
    std::optional<void*> callee_address;
    std::string callee_function_name;
    std::string caller_filename;
    std::optional<unsigned int> caller_line_number;

    // The special members are declared explicitly so they can carry NO_INSTRUMENT.
    // An implicitly-defined destructor / copy / move is generated in whichever
    // translation unit first needs it. For a program that calls get_call_stack()
    // and is itself instrumented, that is the user's TU, where the hooks are on
    // (GCC's exclude-file-list keys on the header that DECLARES a function, and an
    // implicit member has no such header). Every frame destroyed by the user's
    // code then showed up as a "ResolvedFrame::~ResolvedFrame()" trace line.
    // Defaulted on first declaration, they keep the struct's plain
    // field-by-field semantics.
    NO_INSTRUMENT ResolvedFrame() = default;
    NO_INSTRUMENT ~ResolvedFrame() = default;
    NO_INSTRUMENT ResolvedFrame(const ResolvedFrame&) = default;
    NO_INSTRUMENT ResolvedFrame(ResolvedFrame&&) = default;
    NO_INSTRUMENT ResolvedFrame& operator=(const ResolvedFrame&) = default;
    NO_INSTRUMENT ResolvedFrame& operator=(ResolvedFrame&&) = default;
};

// Non-owning counterpart of ResolvedFrame, field for field. This is what the
// enter hook works with on the hot path: the resolver fills the two string
// pointers straight from its memoization caches instead of copying the strings
// into a ResolvedFrame (the function name and, above all, the caller path
// exceed std::string's small-buffer capacity, so each copy was a heap
// allocation on every traced call). `timestamp` is not produced by the
// resolver — the caller points it at its own stack buffer (see
// utils::pretty_time_into) before formatting.
//
// Lifetime: the pointed-to strings live in bfdResolver's name_cache() /
// location_cache(). Both are std::unordered_map (node-based, so a rehash
// never relocates an element), entries are never erased or modified after
// insertion, and the maps themselves are deliberately leaked — the pointers
// therefore stay valid for the rest of the process, and reading through them
// needs no lock. The public API keeps returning owning ResolvedFrame values;
// this view exists for internal use.
struct ResolvedFrameView {
    const char* timestamp = "";
    std::optional<void*> callee_address;
    const std::string* callee_function_name = nullptr;
    const std::string* caller_filename = nullptr;
    std::optional<unsigned int> caller_line_number;
};

} // namespace instrumentation
