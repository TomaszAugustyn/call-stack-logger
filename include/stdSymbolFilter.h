/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <cstring>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace instrumentation {

/**
 * Checks whether a mangled C++ symbol belongs to the standard library or C++ ABI runtime.
 *
 * This runtime filter is necessary ONLY for Clang builds. GCC excludes std library functions
 * at compile time via -finstrument-functions-exclude-file-list (which takes auto-discovered
 * std library header paths). Clang does not support this flag, so std library template
 * instantiations (e.g., std::sort<>, std::vector<>::push_back) compiled into the user's
 * translation unit receive instrumentation hooks and trigger __cyg_profile_func_enter.
 *
 * The re-entrancy guard (in_instrumentation) in trace.cpp only prevents recursive
 * instrumentation from within the resolve/format pipeline. It does NOT prevent std library
 * functions called from user code (where in_instrumentation is false) from being traced.
 * This filter catches those calls by checking the Itanium C++ ABI mangled name for known
 * std library prefixes before the expensive BFD symbol resolution.
 *
 * Matched patterns:
 *   __cxa_*              — C++ ABI runtime (atexit, guard_acquire, etc.)
 *   _Z[N[cv]]St*         — std:: functions and members (St = std:: in Itanium ABI)
 *   _Z[N[cv]]S[absiod]*  — std:: substitutions (allocator, basic_string, string, etc.)
 *   _Z[N[cv]]9__gnu_cxx*     — GNU C++ extensions (__normal_iterator, etc.)
 *   _Z[N[cv]]10__cxxabiv1*   — C++ ABI internals
 *   _Z[N[cv]]11__gnu_debug*  — GNU debug-mode containers (_GLIBCXX_DEBUG)
 *
 * Where [cv] = optional cv-qualifiers: K (const), V (volatile), r (restrict).
 *
 * Pure string parsing with no dependencies — lives in a header (compiled on both
 * compilers, USED only under Clang in callStack.cpp) so unit tests can exercise
 * every pattern directly regardless of the compiler running the test suite.
 */
NO_INSTRUMENT
inline bool is_std_library_symbol(const char* mangled_name) {
    // __cxa_* functions (C++ ABI runtime, e.g., __cxa_atexit, __cxa_guard_acquire).
    // These don't use the _Z mangling prefix so must be checked separately.
    if (std::strncmp(mangled_name, "__cxa_", 6) == 0) {
        return true;
    }
    // All other C++ mangled names start with _Z (Itanium ABI).
    if (mangled_name[0] != '_' || mangled_name[1] != 'Z') {
        return false;
    }
    const char* p = mangled_name + 2;
    // Handle local entities: _ZZ<enclosing-function>E<entity>
    // If the enclosing function is in std::, the local entity is too (e.g.,
    // std::basic_string::_M_construct<>()::_Guard, mangled as _ZZNSt...).
    if (*p == 'Z') {
        p++;
    }
    // Handle nested names: _ZN[cv-qualifiers]...
    // 'N' starts a nested name; K=const, V=volatile, r=restrict are cv-qualifiers.
    if (*p == 'N') {
        p++;
        while (*p == 'K' || *p == 'V' || *p == 'r') {
            p++;
        }
    }
    // Check for std:: (mangled as 'St') and standard substitutions:
    // Sa=allocator, Sb=basic_string, Ss=string, Si=istream, So=ostream, Sd=iostream.
    if (*p == 'S') {
        char next = *(p + 1);
        if (next == 't' || next == 'a' || next == 'b'
            || next == 's' || next == 'i' || next == 'o' || next == 'd') {
            return true;
        }
    }
    // Check for __gnu_cxx:: (mangled as 9__gnu_cxx), __cxxabiv1:: (10__cxxabiv1),
    // and __gnu_debug:: (11__gnu_debug).
    if (std::strncmp(p, "9__gnu_cxx", 10) == 0) {
        return true;
    }
    if (std::strncmp(p, "10__cxxabiv1", 12) == 0) {
        return true;
    }
    if (std::strncmp(p, "11__gnu_debug", 13) == 0) {
        return true;
    }
    return false;
}

} // namespace instrumentation
