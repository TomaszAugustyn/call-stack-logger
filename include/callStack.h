/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include "types.h"
#include <bfd.h>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace instrumentation {

/**
 * Loads symbols and resolves address pointers to ResolvedFrame
 *
 * It uses binutils to define bfds, caches the bfds to read them only once
 * per run of the application. For the use of dladdr see also:
 * https://sourceware.org/git/?p=glibc.git;a=blob;f=debug/backtracesyms.c
 */
struct bfdResolver {
public:
    /// Single bfd structure
    struct storedBfd {
        // Deleter type derived from bfd_close itself rather than spelled with
        // bfd_boolean: upstream binutils removed the bfd_boolean typedef (>= 2.38);
        // current distro headers only provide it as a compat #define that can be
        // poisoned via POISON_BFD_BOOLEAN. decltype tracks whatever return type
        // the installed bfd.h declares (bfd_boolean, bool, int, ...).
        using deleter_t = decltype(bfd_close);

        // Custom deleter that casts back to char* before delete[], matching the
        // allocation type (new char[]) used for the BFD symbol table storage.
        struct SymbolDeleter {
            NO_INSTRUMENT void operator()(asymbol** p) const { delete[] reinterpret_cast<char*>(p); }
        };

        std::unique_ptr<bfd, deleter_t*> abfd;
        std::unique_ptr<asymbol*[], SymbolDeleter> symbols;
        intptr_t offset;

        NO_INSTRUMENT storedBfd(bfd* _abfd, deleter_t* _del) : abfd(_abfd, _del) {}
        // Explicit move operations and destructor with NO_INSTRUMENT prevent the
        // compiler-generated special members from being instrumented during static
        // cleanup of s_bfds. Declaring a destructor suppresses implicit move
        // generation in C++, so move operations must be explicitly defaulted.
        NO_INSTRUMENT storedBfd(storedBfd&&) = default;
        NO_INSTRUMENT storedBfd& operator=(storedBfd&&) = default;
        NO_INSTRUMENT ~storedBfd() = default;
    };

    /// Returns the cached (or freshly loaded) storedBfd for the object containing
    /// `_info`, or nullptr when the object file cannot be opened/parsed. Pointer
    /// stays valid for the process lifetime (s_bfds entries are never erased).
    NO_INSTRUMENT
    static storedBfd* ensure_bfd_loaded(Dl_info& _info);

    /// Resolves callee + caller using the unwinder to find the actual user-code
    /// caller from inside the instrumentation pipeline. The `caller_address`
    /// parameter is the call site passed by `__cyg_profile_func_enter`; the
    /// unwinder walks up the fixed depth of the resolve pipeline to reach the
    /// real caller. Use this from instrumentation hooks.
    NO_INSTRUMENT
    static std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address);

    /// Resolves callee + caller using both addresses verbatim — does NOT run the
    /// unwinder. Use this when the caller is the actual user-code call site
    /// (e.g. from `get_call_stack()`, where backtrace() already provides the
    /// per-frame return addresses).
    NO_INSTRUMENT
    static std::optional<ResolvedFrame> resolve_no_unwind(
            void* callee_address, void* caller_address);

private:
    NO_INSTRUMENT
    static std::optional<std::string> resolve_function_name(void* callee_address);

    NO_INSTRUMENT
    static std::pair<std::string, std::optional<unsigned int>> resolve_filename_and_line(
            void* caller_address);

    NO_INSTRUMENT
    static void check_bfd_initialized();

    NO_INSTRUMENT
    static std::string get_argv0();

    NO_INSTRUMENT
    static void ensure_actual_executable(Dl_info& symbol_info);

    inline static std::map<void*, storedBfd> s_bfds = {};
    // Negative cache: base addresses whose object file failed to load/parse.
    // Without it, every traced call whose callee or caller lands in an
    // unloadable object (deleted .so, unreadable file, unparsable format)
    // would re-run bfd_openr + bfd_check_format — file I/O per call. Bounded
    // by the number of distinct loaded objects. Protected by s_bfd_mutex.
    inline static std::unordered_set<void*> s_bfd_load_failed = {};
    // Memoization caches for fully-resolved per-address results, consulted in
    // resolve_no_unwind() before the BFD machinery. s_bfds caches the PARSED
    // OBJECT FILES, but each resolve still cost dladdr + a section walk +
    // bfd_find_nearest_line (a DWARF line-table walk) + __cxa_demangle — per
    // call, repeated in full every time the same function was called again.
    // Memoizing by exact address turns repeat resolutions into one hash lookup.
    // A cached nullopt in s_name_cache is meaningful: it records "this callee
    // is filtered / not loggable" (e.g. Clang's std-library filter), making the
    // filter itself a hash hit on repeat calls.
    //
    // Growth is bounded by the program text, not by runtime input: keys are
    // code addresses, so distinct callees ≤ number of instrumented functions
    // and distinct callers ≤ number of call sites in the loaded code — both
    // fixed at link/load time. Typical programs: thousands of entries (hundreds
    // of KB); worst realistic case for very large binaries: a few hundred
    // thousand entries (tens of MB). Staleness caveat: dlclose + dlopen that
    // reuses an address keeps serving the old entry — the same accepted
    // trade-off as s_bfds. Protected by s_bfd_mutex.
    inline static std::unordered_map<void*, std::optional<std::string>> s_name_cache = {};
    inline static std::unordered_map<void*, std::pair<std::string, std::optional<unsigned int>>>
            s_location_cache = {};
    inline static bool s_bfd_initialized = false;
    inline static std::string s_argv0 = get_argv0();
    // Protects s_bfds, s_bfd_initialized, and BFD library calls which are not thread-safe.
    inline static std::mutex s_bfd_mutex;
};

/**
 * Returns a vector of the ResolvedFrames representing current call-stack.
 *
 * The depth of the stack is by default 1000. Throws runtime_error when call stack cannot be built.
 */
NO_INSTRUMENT
std::vector<std::optional<ResolvedFrame>> get_call_stack();

/// Returns the ResolvedFrame if address resolution succeeds, std::nullopt if fails.
NO_INSTRUMENT
std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address);

} // namespace instrumentation
