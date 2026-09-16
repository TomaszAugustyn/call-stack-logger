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

} // namespace instrumentation
