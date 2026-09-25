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
//
// Only macros WE defined are #undef'd right after the include: leaving empty
// definitions behind would clash with consumer TUs that define their own
// PACKAGE afterwards (e.g. an autotools-generated config.h with
// `#define PACKAGE "myapp"` — a macro redefinition), while a consumer's
// pre-existing definition is passed through untouched.
#ifndef PACKAGE
    #define PACKAGE
    #define CSLG_UNDEF_PACKAGE
#endif
#ifndef PACKAGE_VERSION
    #define PACKAGE_VERSION
    #define CSLG_UNDEF_PACKAGE_VERSION
#endif

#include <bfd.h>

#ifdef CSLG_UNDEF_PACKAGE
    #undef PACKAGE
    #undef CSLG_UNDEF_PACKAGE
#endif
#ifdef CSLG_UNDEF_PACKAGE_VERSION
    #undef PACKAGE_VERSION
    #undef CSLG_UNDEF_PACKAGE_VERSION
#endif

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
        // Assigned by ensure_bfd_loaded() just before the map emplace; the
        // zero-init keeps the member from ever being read indeterminate.
        intptr_t offset = 0;

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

    /// Resolves callee + caller for the instrumentation hooks. `caller_address`
    /// is the hook's second argument: both GCC and Clang pass the instrumented
    /// function's own return address (`__builtin_return_address(0)`), i.e. the
    /// address just after the call instruction inside the REAL caller. This
    /// steps one byte back into that call instruction (so the line lookup lands
    /// on the call, not on whatever follows it) and resolves both addresses.
    /// Returns false when the callee is filtered / not loggable; on success `out`
    /// points straight into the memoization caches (no string copies — see
    /// ResolvedFrameView for the lifetime guarantee) with `timestamp` left for
    /// the caller to fill.
    NO_INSTRUMENT
    static bool resolve(void* callee_address, void* caller_address, ResolvedFrameView& out);

    /// Resolves callee + caller using both addresses verbatim — no return-address
    /// adjustment. Use this when `caller_address` already points inside the call
    /// instruction (e.g. from `get_call_stack()`, which applies the same one-byte
    /// step-back to backtrace()'s per-frame return addresses itself). Copies the
    /// cached strings into an owning ResolvedFrame and stamps it with the current
    /// time.
    NO_INSTRUMENT
    static std::optional<ResolvedFrame> resolve_no_unwind(
            void* callee_address, void* caller_address);

    /// Non-owning form of resolve_no_unwind(): the memoized (or, on first
    /// sight, freshly resolved) callee name and caller location are returned as
    /// pointers into the caches. This is the only function that touches the
    /// caches together with resolve_location() below, so s_bfd_mutex is held
    /// exactly there and nowhere up the stack.
    NO_INSTRUMENT
    static bool resolve_no_unwind(void* callee_address, void* caller_address, ResolvedFrameView& out);

    /// The location half of resolve_no_unwind() on its own: fills
    /// `out.caller_filename` / `out.caller_line_number` for `address` (an
    /// instruction address taken verbatim, e.g. a throw site) through the same
    /// memoized lookup call sites use, including its fallback texts. The other
    /// fields of `out` are left untouched.
    NO_INSTRUMENT
    static void resolve_location(void* address, ResolvedFrameView& out);

    /// The inline chain at `address`: the base names (function_base_name in
    /// frameReconcile.h) of the functions inlined into each other there,
    /// innermost first, read from DWARF with bfd_find_inliner_info(). Empty when
    /// the address has no DWARF line information (then nothing is known about
    /// inlining there). Memoized per address; the pointer stays valid for the
    /// process lifetime like the other cache entries.
    NO_INSTRUMENT
    static const std::vector<std::string>* resolve_inline_chain(void* address);

private:
    /// Walks the object's section list to find the section containing `address`
    /// and writes the section-relative offset to `offset_out`. Returns nullptr
    /// when no section contains the address. Shared by resolve_function_name()
    /// and resolve_filename_and_line(). Must be called with s_bfd_mutex held.
    NO_INSTRUMENT
    static asection* find_containing_section(storedBfd& currBfd, void* address,
                                             intptr_t& offset_out);

    /// Names the callee from `dl_info`, the dladdr() result for callee_address
    /// (nullptr when dladdr() failed). Must be called with s_bfd_mutex held; the
    /// dladdr() call itself is made by resolve_no_unwind() OUTSIDE the mutex —
    /// see the lock-order note there.
    NO_INSTRUMENT
    static std::optional<std::string> resolve_function_name(void* callee_address,
                                                            const Dl_info* dl_info);

    /// One memoized callee name: the demangled name (nullopt = filtered / not
    /// loggable) and its base name (function_base_name in frameReconcile.h,
    /// used by the frame reconciliation), computed once here so the enter hook
    /// never parses a name on the hot path.
    struct CachedName {
        std::optional<std::string> name;
        std::string base;
    };

    /// One memoized call-site location: the file (or a fallback text), the line
    /// (none when unknown) and the base name of the innermost function containing
    /// the address (empty when unknown; see ResolvedFrameView::caller_function_base).
    struct CachedLocation {
        std::string file;
        std::optional<unsigned int> line;
        std::string function_base;
    };

    /// Same contract as resolve_function_name(): `dl_info` is the caller's
    /// dladdr() result, obtained outside s_bfd_mutex.
    NO_INSTRUMENT
    static CachedLocation resolve_filename_and_line(void* caller_address, const Dl_info* dl_info);

    /// Reads the inline chain for `address` (see resolve_inline_chain) — must be
    /// called with s_bfd_mutex held; `dl_info` is the address's dladdr() result
    /// obtained outside the mutex (null when dladdr() failed).
    NO_INSTRUMENT
    static std::vector<std::string> read_inline_chain(void* address, const Dl_info* dl_info);

    NO_INSTRUMENT
    static void check_bfd_initialized();

    NO_INSTRUMENT
    static std::string get_argv0();

    /// Absolute path of the file mapped at `base`, taken from /proc/self/maps
    /// (the kernel records it at mmap time, so it stays valid after chdir()).
    /// nullopt when no file-backed mapping contains `base`.
    NO_INSTRUMENT
    static std::optional<std::string> mapped_object_path(void* base);

    /// Path to hand to bfd_openr for the object described by `symbol_info`:
    /// /proc/self/exe for the main executable, else the mapped file's absolute
    /// path, else dli_fname verbatim. Empty when nothing usable exists.
    NO_INSTRUMENT
    static std::string object_file_path(const Dl_info& symbol_info);

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
    // and do hash lookups — negligible.
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
    static std::unordered_map<void*, CachedName>& name_cache() {
        static auto* instance = new std::unordered_map<void*, CachedName>();
        return *instance;
    }

    /// Wraps a freshly resolved callee name for the name cache.
    NO_INSTRUMENT
    static CachedName cache_name(std::optional<std::string> name);

    NO_INSTRUMENT
    static std::unordered_map<void*, CachedLocation>& location_cache() {
        static auto* instance = new std::unordered_map<void*, CachedLocation>();
        return *instance;
    }

    /// Inline chains per address (resolve_inline_chain). Consulted only when
    /// the frame reconciliation meets records at the current frame's own level
    /// (inlined activations, catches), so it stays small. Leaked and protected
    /// by s_bfd_mutex like the other caches.
    NO_INSTRUMENT
    static std::unordered_map<void*, std::vector<std::string>>& inline_chain_cache() {
        static auto* instance = new std::unordered_map<void*, std::vector<std::string>>();
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
    // library calls which are not thread-safe. Lock-order rule: nothing that
    // takes glibc's loader lock — dladdr() above all — may run while this
    // mutex is held; see resolve_no_unwind() in callStack.cpp for why.
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
/// `caller_address` is the return address as passed by `__cyg_profile_func_enter`
/// (see bfdResolver::resolve).
NO_INSTRUMENT
std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address);

/// Allocation-free form used by the enter hook: fills `out` with pointers into
/// the resolver's caches (see ResolvedFrameView) instead of copying strings.
/// Returns false when the frame is filtered / not loggable. `caller_address` is
/// the hook's return-address argument; the one-byte step-back into the call
/// instruction happens inside.
NO_INSTRUMENT
bool resolve(void* callee_address, void* caller_address, ResolvedFrameView& out);

/// Resolves the source location of `address` — an instruction address taken
/// verbatim, such as the site of a throw — into `out.caller_filename` /
/// `out.caller_line_number`, exactly as call sites are resolved (memoized,
/// with the same fallback texts). Used for the LOG_EXCEPTIONS event lines.
NO_INSTRUMENT
void resolve_site(void* address, ResolvedFrameView& out);

/// Demangles an Itanium C++ ABI symbol (a type_info name, a function symbol);
/// returns the input unchanged when it is not a mangled name.
NO_INSTRUMENT
std::string demangle_symbol(const char* mangled);

/// The inline chain at `address` (see bfdResolver::resolve_inline_chain), for
/// the frame reconciliation of frames that share a physical frame.
NO_INSTRUMENT
const std::vector<std::string>* inline_chain_at(void* address);

} // namespace instrumentation
