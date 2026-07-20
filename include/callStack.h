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

// Workaround for deliberately incompatible bfd.h header files on some systems.
// binutils < 2.39 (e.g. Ubuntu 22.04, Debian 11) #errors out unless PACKAGE /
// PACKAGE_VERSION are defined — without this guard, consumers of the public
// get_call_stack() API could not include this header on those distros.
// Mirrors the same workaround in src/callStack.cpp.
#ifndef PACKAGE
    #define PACKAGE
#endif
#ifndef PACKAGE_VERSION
    #define PACKAGE_VERSION
#endif

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

// Applied to every function in the fixed call chain behind the frame-6 constant
// (see bfdResolver::resolve in callStack.cpp). Without it, an optimized build of
// the library (-O2, e.g. CMAKE_BUILD_TYPE=RelWithDebInfo — the build type the
// README recommends to integrators) inlines parts of the chain, the hard-coded
// frame count no longer matches the real stack, and every trace line silently
// reports a wrong caller. NO_INLINE keeps the chain shape identical at every
// optimization level; the cost is one genuine call per chain function per traced
// call — noise next to the resolve pipeline itself.
#ifndef NO_INLINE
    #define NO_INLINE __attribute__((noinline))
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
        // Explicit move operations and destructor with NO_INSTRUMENT keep the
        // compiler-generated special members uninstrumented (they run during
        // emplace into the bfds() map and on the failure paths in
        // ensure_bfd_loaded; the map itself is deliberately leaked, so there is
        // no static cleanup). Declaring a destructor suppresses implicit move
        // generation in C++, so move operations must be explicitly defaulted.
        NO_INSTRUMENT storedBfd(storedBfd&&) = default;
        NO_INSTRUMENT storedBfd& operator=(storedBfd&&) = default;
        NO_INSTRUMENT ~storedBfd() = default;
    };

    /// Returns the cached (or freshly loaded) storedBfd for the object containing
    /// `_info`, or nullptr when the object file cannot be opened/parsed. Pointer
    /// stays valid for the process lifetime (bfds() entries are never erased).
    NO_INSTRUMENT
    static storedBfd* ensure_bfd_loaded(Dl_info& _info);

    /// Resolves callee + caller using the unwinder to find the actual user-code
    /// caller from inside the instrumentation pipeline. The `caller_address`
    /// parameter is the call site passed by `__cyg_profile_func_enter`; the
    /// unwinder walks up the fixed depth of the resolve pipeline to reach the
    /// real caller. Use this from instrumentation hooks.
    /// NO_INLINE: frame 3 of the fixed chain behind the frame-6 constant.
    NO_INSTRUMENT NO_INLINE
    static std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address);

    /// Resolves callee + caller using both addresses verbatim — does NOT run the
    /// unwinder. Use this when the caller is the actual user-code call site
    /// (e.g. from `get_call_stack()`, where backtrace() already provides the
    /// per-frame return addresses).
    NO_INSTRUMENT
    static std::optional<ResolvedFrame> resolve_no_unwind(
            void* callee_address, void* caller_address);

private:
    /// Returns true when name_cache() already records this callee as filtered
    /// (cached nullopt: Clang std-library filter, internal-linkage functions
    /// with no dladdr symbol, failed demangling). Used by resolve() to skip
    /// the _Unwind_Backtrace walk for callees that can never produce a trace
    /// line — the unwind exists only to key the caller-location lookup, which
    /// such callees never reach. Takes s_bfd_mutex internally.
    NO_INSTRUMENT
    static bool is_cached_filtered(void* callee_address);

    /// Walks the object's section list to find the section containing `address`
    /// and writes the section-relative offset to `offset_out`. Returns nullptr
    /// when no section contains the address. Shared by resolve_function_name()
    /// and resolve_filename_and_line(). Must be called with s_bfd_mutex held.
    NO_INSTRUMENT
    static asection* find_containing_section(storedBfd& currBfd, void* address,
                                             intptr_t& offset_out);

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

    // Static-state accessors. Function-local statics rather than namespace-scope
    // inline statics, deliberately: containers and std::string require DYNAMIC
    // initialization, and initialization order across translation units is
    // unspecified. An instrumented static constructor in a user TU that runs
    // after trace_begin() (so trace_ready is already true) but before this TU's
    // dynamic initializers would reach resolve() through unconstructed maps —
    // UB. Function-local statics are constructed on first use (thread-safely,
    // per C++11 magic statics), closing that window. Same rationale as the
    // g_trace() singleton in trace.cpp. The per-call cost is one
    // already-initialized guard check on paths that already take s_bfd_mutex
    // and do hash lookups — negligible, and none of these accessors run while
    // the unwinder walks the stack (resolve_no_unwind() and is_cached_filtered()
    // either run after the unwind or return before it), so the frame-6 constant
    // is unaffected.
    //
    // Each instance is heap-allocated and deliberately LEAKED (never destroyed),
    // again mirroring g_trace(): these caches are first used during tracing,
    // i.e. AFTER trace_begin() registered trace_shutdown via atexit, and exit
    // handlers run in reverse registration order — so with plain function-local
    // statics their destructors would run BEFORE trace_shutdown. In that window
    // shutdown_complete is not yet set, so a worker thread can still be inside
    // resolve() mutating these maps while the exiting thread destroys them — a
    // use-after-free that defeats the documented "torn final line at worst"
    // shutdown guarantee. (Under Clang the map destructors themselves could
    // additionally fire hooks through instrumented COMDAT std internals from a
    // user TU, mid-destruction.) Leaking removes static destruction of resolver
    // state entirely: the kernel reclaims the memory at process exit, LSan
    // treats reachable globals as live (nothing is reported), and the skipped
    // bfd_close calls only release read-only descriptors the kernel closes
    // anyway.
    //
    // s_bfd_mutex and s_bfd_initialized stay as plain statics below: std::mutex
    // has a constexpr constructor and bool is zero-initialized, so both are
    // constant-initialized before any code runs — no order hazard.
    NO_INSTRUMENT
    static std::map<void*, storedBfd>& bfds() {
        static auto* instance = new std::map<void*, storedBfd>();
        return *instance;
    }

    /// Negative cache: base addresses whose object file failed to load/parse.
    /// Without it, every traced call whose callee or caller lands in an
    /// unloadable object (deleted .so, unreadable file, unparsable format)
    /// would re-run bfd_openr + bfd_check_format — file I/O per call. Bounded
    /// by the number of distinct loaded objects. Protected by s_bfd_mutex.
    NO_INSTRUMENT
    static std::unordered_set<void*>& bfd_load_failed() {
        static auto* instance = new std::unordered_set<void*>();
        return *instance;
    }

    // Memoization caches for fully-resolved per-address results, consulted in
    // resolve_no_unwind() before the BFD machinery. bfds() caches the PARSED
    // OBJECT FILES, but each resolve still cost dladdr + a section walk +
    // bfd_find_nearest_line (a DWARF line-table walk) + __cxa_demangle — per
    // call, repeated in full every time the same function was called again.
    // Memoizing by exact address turns repeat resolutions into one hash lookup.
    // A cached nullopt in name_cache() is meaningful: it records "this callee
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
    // trade-off as bfds(). Protected by s_bfd_mutex.
    NO_INSTRUMENT
    static std::unordered_map<void*, std::optional<std::string>>& name_cache() {
        static auto* instance = new std::unordered_map<void*, std::optional<std::string>>();
        return *instance;
    }

    NO_INSTRUMENT
    static std::unordered_map<void*, std::pair<std::string, std::optional<unsigned int>>>&
    location_cache() {
        static auto* instance =
                new std::unordered_map<void*, std::pair<std::string, std::optional<unsigned int>>>();
        return *instance;
    }

    /// argv[0] as read from /proc/self/cmdline. Lazily initialized on first
    /// resolve (programs that never resolve don't read the file at all).
    NO_INSTRUMENT
    static const std::string& argv0() {
        static const std::string* instance = new std::string(get_argv0());
        return *instance;
    }

    inline static bool s_bfd_initialized = false;
    // Protects bfds(), s_bfd_initialized, the memoization caches, and BFD
    // library calls which are not thread-safe.
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
/// NO_INLINE: frame 4 of the fixed chain behind the frame-6 constant (relevant
/// under LTO, where even the cross-TU call from the enter hook could inline).
NO_INSTRUMENT NO_INLINE
std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address);

} // namespace instrumentation
