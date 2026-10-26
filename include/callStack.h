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

#include <cstdint>
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

// The symbolizer behind the resolver is libbacktrace (vendored, see
// third_party/libbacktrace). Its state is opaque here: this header is part of
// the public API and must not require libbacktrace's headers in a consumer's
// build, so the state is only ever named through this forward declaration and
// touched in callStack.cpp.
struct backtrace_state;

namespace instrumentation {

/**
 * Loads symbols and resolves address pointers to ResolvedFrame
 *
 * Every object file the program has loaded (the executable, its shared
 * libraries, dlopen()ed plugins) is registered once, at first sight of an
 * address inside it, with one libbacktrace state at the object's runtime load
 * bias; the state then answers symbol-table and DWARF lookups for runtime
 * addresses directly. The object behind an address comes from dladdr and
 * /proc/self/maps. For the use of dladdr see also:
 * https://sourceware.org/git/?p=glibc.git;a=blob;f=debug/backtracesyms.c
 */
struct addressResolver {
public:
    /// What the resolver knows about one registered object file (an entry of
    /// objects(), keyed by the object's dli_fbase).
    struct ObjectFile {
        /// link_map::l_addr — the base libbacktrace adds to the file's
        /// addresses. 0 for a non-PIE executable, the map start otherwise.
        uintptr_t load_bias = 0;
        /// The file (or its separate debug file) has a symbol table / DWARF
        /// debug information; lookups that cannot succeed are skipped.
        bool has_symtab = false;
        bool has_dwarf = false;
        /// The file itself carries a .symtab section (a full strip removes it;
        /// `has_symtab` is then true only through .dynsym).
        bool has_symtab_section = false;
    };

    /// Returns the cached (or freshly registered) ObjectFile for the object
    /// containing `_info` (its dladdr() result; `load_bias` is the object's
    /// link_map::l_addr obtained with it), or nullptr when the object file
    /// cannot be opened/parsed. Pointer stays valid for the process lifetime
    /// (objects() entries are never erased). Must be called with
    /// s_resolver_mutex held.
    NO_INSTRUMENT
    static ObjectFile* ensure_object_loaded(const Dl_info& _info, uintptr_t load_bias);

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
    /// instruction. Copies the cached strings into an owning ResolvedFrame and
    /// stamps it with the current time. Names by the innermost function (the
    /// hooks' rule); get_call_stack() uses resolve_physical_frame() instead.
    NO_INSTRUMENT
    static std::optional<ResolvedFrame> resolve_no_unwind(
            void* callee_address, void* caller_address);

    /// Non-owning form of resolve_no_unwind(): the memoized (or, on first
    /// sight, freshly resolved) callee name and caller location are returned as
    /// pointers into the caches. This is the only function that touches the
    /// caches together with resolve_location() below, so s_resolver_mutex is
    /// held exactly there and nowhere up the stack.
    /// Which function names a frame when the address lies inside inlined code.
    /// The hooks name a callee from its ENTRY address, where the innermost
    /// function of DWARF's inline chain is the function itself — BFD's choice,
    /// kept byte for byte. get_call_stack() names a PHYSICAL frame from a return
    /// address inside it, where the innermost function is often a helper
    /// inlined right after the call (a vector's iterator or move constructor,
    /// an ostringstream's constructor); the frame's own function is the
    /// OUTERMOST one of the chain, so that is what names it. `name_of` selects
    /// the rule and with it the name cache (name_cache() for the hooks,
    /// frame_name_cache() for physical frames); the location half is the same.
    enum class NameOf { innermost_function, outermost_function };

    NO_INSTRUMENT
    static bool resolve_no_unwind(void* callee_address, void* caller_address, ResolvedFrameView& out,
                                  NameOf name_of = NameOf::innermost_function);

    /// One frame of get_call_stack(): `return_address` is what backtrace()
    /// reported for the frame — the instruction after the call the frame made —
    /// and `caller_address` the call site that created the frame, already
    /// stepped back into its call instruction. The frame is named after the
    /// OUTERMOST function at `return_address - 1` (NameOf::outermost_function;
    /// the step back keeps a call that ends a function from being attributed to
    /// the function that follows it). Copies the result into an owning frame.
    NO_INSTRUMENT
    static std::optional<ResolvedFrame> resolve_physical_frame(void* return_address, void* caller_address);

    /// The location half of resolve_no_unwind() on its own: fills
    /// `out.caller_filename` / `out.caller_line_number` for `address` (an
    /// instruction address taken verbatim, e.g. a throw site) through the same
    /// memoized lookup call sites use, including its fallback texts. The other
    /// fields of `out` are left untouched.
    NO_INSTRUMENT
    static void resolve_location(void* address, ResolvedFrameView& out);

    /// The inline chain at `address`: the base names (function_base_name in
    /// frameReconcile.h) of the functions inlined into each other there,
    /// innermost first, read from DWARF (libbacktrace reports one entry per
    /// inlined level, innermost first). Empty when the address has no DWARF
    /// line information (then nothing is known about inlining there). Memoized
    /// per address; the pointer stays valid for the process lifetime like the
    /// other cache entries.
    NO_INSTRUMENT
    static const std::vector<std::string>* resolve_inline_chain(void* address);

    /// The dladdr() result for an address plus the containing object's load
    /// bias, both obtained OUTSIDE s_resolver_mutex (dladdr takes glibc's
    /// loader lock — see the lock-order note in resolve_no_unwind()). `valid`
    /// is false when dladdr() failed; the other fields are then unspecified.
    struct AddressInfo {
        Dl_info info {};
        uintptr_t load_bias = 0;
        bool valid = false;
    };

    /// dladdr1(RTLD_DL_LINKMAP) for `address`: the Dl_info and the link_map's
    /// l_addr, read while the map is certainly alive. Never call with
    /// s_resolver_mutex held.
    NO_INSTRUMENT
    static AddressInfo query_address(void* address);

    /// One entry libbacktrace reports for an address: the function containing
    /// it (its DWARF name, a linkage name where DWARF has one) and the source
    /// position. For an address inside inlined code there is one entry per
    /// inlined level, innermost first; `file`/`line` of an entry are the
    /// position inside that function (for all but the innermost: the call site
    /// of the next inner one). Empty strings / line 0 mean "not available".
    struct DwarfEntry {
        std::string function;
        std::string file;
        unsigned int line = 0;
    };

    /// libbacktrace's complete answer for one address, from the DWARF debug
    /// information (`entries`, innermost first; empty when no registered file
    /// describes the address) and from the symbol table (`symbol`, the
    /// function or object symbol containing the address, `symbol_value` its
    /// start; empty when none).
    struct Lookup {
        std::vector<DwarfEntry> entries;
        std::string symbol;
        uintptr_t symbol_value = 0;
    };

private:
    /// Runs backtrace_pcinfo() and backtrace_syminfo() for `address` (a
    /// runtime address inside `object`). Must be called with s_resolver_mutex
    /// held: libbacktrace's lazy per-unit parsing may otherwise run twice.
    NO_INSTRUMENT
    static Lookup look_up(const ObjectFile& object, void* address);

    /// The name of the function containing the address — the innermost or the
    /// outermost of DWARF's inline chain, per `name_of` — chosen as BFD's
    /// bfd_find_nearest_line() chose it (the resolver of releases before
    /// v1.0.0), so that the printed names stay what they were with libbfd:
    /// the DWARF linkage name when there is one, otherwise the symbol table's
    /// name (which carries the mangled parameter list for a static function,
    /// an anonymous-namespace function or a lambda), unless that only repeats
    /// the DWARF name or adds a clone suffix to it. Empty when nothing names
    /// the address.
    NO_INSTRUMENT
    static std::string function_name_of(const Lookup& lookup, NameOf name_of);

    /// The base name (function_base_name in frameReconcile.h) of the innermost
    /// function at the address, for the frame reconciliation only — it is never
    /// printed. Taken from DWARF whenever DWARF names the function: inside
    /// inlined code the symbol table only knows the inline HOST, and
    /// function_name_of() prefers the symbol table unless DWARF gives a mangled
    /// linkage name, which no C function has and, on GCC, no internal-linkage
    /// C++ function (static, anonymous namespace, lambda) — it would name the
    /// host there. Falls back to function_name_of() (symbol table, then
    /// nothing) when DWARF has no name for the address. Empty when nothing
    /// names it.
    NO_INSTRUMENT
    static std::string innermost_base_name(const Lookup& lookup);

    /// Names the callee from `address_info`, the dladdr() result for
    /// callee_address (`valid` false when dladdr() failed). Must be called
    /// with s_resolver_mutex held; the dladdr() call itself is made by
    /// resolve_no_unwind() OUTSIDE the mutex — see the lock-order note there.
    NO_INSTRUMENT
    static std::optional<std::string> resolve_function_name(void* callee_address,
                                                            const AddressInfo& address_info,
                                                            NameOf name_of);

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

    /// Same contract as resolve_function_name(): `address_info` is the caller's
    /// dladdr() result, obtained outside s_resolver_mutex.
    NO_INSTRUMENT
    static CachedLocation resolve_filename_and_line(void* caller_address, const AddressInfo& address_info);

    /// Location of an exception event's site (the interposer's return address
    /// minus one): resolve_filename_and_line() plus a check that the line row
    /// covering the address did not leak across a symbol boundary from the code
    /// before it (GCC 13 at -O2 gives a landing pad's entry no location of its
    /// own — see the definition), with the handler's first statement as the
    /// fallback. Must be called with s_resolver_mutex held.
    static CachedLocation resolve_event_site_location(void* address, const AddressInfo& address_info);

    /// Reads the inline chain for `address` (see resolve_inline_chain) — must be
    /// called with s_resolver_mutex held; `address_info` is the address's dladdr()
    /// result obtained outside the mutex.
    NO_INSTRUMENT
    static std::vector<std::string> read_inline_chain(void* address, const AddressInfo& address_info);

    /// The one libbacktrace state, created on first use under s_resolver_mutex
    /// (threaded, with the lazy executable discovery disabled — see
    /// src/backtraceExtension.c) and never freed: libbacktrace has no
    /// counterpart to backtrace_create_state(), and the caches below point into
    /// it for the process lifetime anyway. nullptr when creation failed; every
    /// lookup then degrades to "no information".
    NO_INSTRUMENT
    static backtrace_state* state();

    NO_INSTRUMENT
    static std::string get_argv0();

    /// Absolute path of the file mapped at `base`, taken from /proc/self/maps
    /// (the kernel records it at mmap time, so it stays valid after chdir()).
    /// nullopt when no file-backed mapping contains `base`.
    NO_INSTRUMENT
    static std::optional<std::string> mapped_object_path(void* base);

    /// Path of the object file described by `symbol_info` to register with
    /// libbacktrace: /proc/self/exe for the main executable, else the mapped
    /// file's absolute path, else dli_fname when it is absolute. Empty when
    /// nothing usable exists — also for a mapping whose file was deleted, and
    /// for the vdso (whose dli_fname is a bare name).
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
    // already-initialized guard check on paths that already take
    // s_resolver_mutex and do hash lookups — negligible.
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
    // state entirely: the kernel reclaims the memory at process exit and LSan
    // treats reachable globals as live (nothing is reported). libbacktrace's
    // own memory comes from mmap and is never freed either, by its design.
    //
    // s_resolver_mutex stays a plain static below: std::mutex has a constexpr
    // constructor, so it is constant-initialized before any code runs — no
    // order hazard.
    NO_INSTRUMENT
    static std::map<void*, ObjectFile>& objects() {
        static auto* instance = new std::map<void*, ObjectFile>();
        return *instance;
    }

    /// Negative cache: base addresses whose object file failed to open/parse.
    /// Without it, every traced call whose callee or caller lands in an
    /// unloadable object (deleted .so, unreadable or truncated file, unparsable
    /// format) would re-run the open and the ELF checks — file I/O per call.
    /// Bounded by the number of distinct loaded objects. Protected by
    /// s_resolver_mutex.
    NO_INSTRUMENT
    static std::unordered_set<void*>& object_load_failed() {
        static auto* instance = new std::unordered_set<void*>();
        return *instance;
    }

    // Memoization caches for fully-resolved per-address results, consulted in
    // resolve_no_unwind() before libbacktrace. objects() remembers which
    // files are registered, but each resolve would still cost dladdr + a
    // DWARF line-table walk + a symbol-table search + __cxa_demangle — per
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
    // trade-off as objects(). Protected by s_resolver_mutex.
    NO_INSTRUMENT
    static std::unordered_map<void*, CachedName>& name_cache() {
        static auto* instance = new std::unordered_map<void*, CachedName>();
        return *instance;
    }

    /// Wraps a freshly resolved callee name for the name cache.
    NO_INSTRUMENT
    static CachedName cache_name(std::optional<std::string> name);

    /// The names of PHYSICAL frames for get_call_stack(), keyed by the frame's
    /// return address minus one and named by the outermost function there
    /// (NameOf::outermost_function). Kept apart from name_cache(), whose
    /// entries name the innermost function at a callee's entry address: the
    /// two rules answer differently inside inlined code, and the hooks' cache
    /// stays exactly what it was. Bounded by the call sites a captured stack
    /// can contain; leaked and protected by s_resolver_mutex like the others.
    NO_INSTRUMENT
    static std::unordered_map<void*, CachedName>& frame_name_cache() {
        static auto* instance = new std::unordered_map<void*, CachedName>();
        return *instance;
    }

    NO_INSTRUMENT
    static std::unordered_map<void*, CachedLocation>& location_cache() {
        static auto* instance = new std::unordered_map<void*, CachedLocation>();
        return *instance;
    }

    /// Inline chains per address (resolve_inline_chain). Consulted only when
    /// the frame reconciliation meets records at the current frame's own level
    /// (inlined activations, catches), so it stays small. Leaked and protected
    /// by s_resolver_mutex like the other caches.
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

    // Protects objects(), the libbacktrace state and every call into it (its
    // lazy per-unit parsing in threaded mode lets two threads both parse and
    // leaks the loser's work), and the memoization caches. Lock-order rule:
    // nothing that takes glibc's loader lock — dladdr() above all — may run
    // while this mutex is held; see resolve_no_unwind() in callStack.cpp for why.
    inline static std::mutex s_resolver_mutex;
};

/**
 * Returns a vector of the ResolvedFrames representing current call-stack.
 *
 * There is no depth limit (room for 1000 frames first, doubled as needed). Throws runtime_error when the
 * call stack cannot be built.
 */
NO_INSTRUMENT
std::vector<std::optional<ResolvedFrame>> get_call_stack();

/// Returns the ResolvedFrame if address resolution succeeds, std::nullopt if fails.
/// `caller_address` is the return address as passed by `__cyg_profile_func_enter`
/// (see addressResolver::resolve).
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

/// The inline chain at `address` (see addressResolver::resolve_inline_chain), for
/// the frame reconciliation of frames that share a physical frame.
NO_INSTRUMENT
const std::vector<std::string>* inline_chain_at(void* address);

} // namespace instrumentation
