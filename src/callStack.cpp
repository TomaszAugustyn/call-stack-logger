/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#include "callStack.h"
#include "prettyTime.h"
#include "stdSymbolFilter.h"
#include "unwinder.h"

// Workaround for deliberately incompatible bfd.h header files on some systems.
#ifndef PACKAGE
    #define PACKAGE
#endif
#ifndef PACKAGE_VERSION
    #define PACKAGE_VERSION
#endif

#include <bfd.h>
#include <cxxabi.h> // for __cxa_demangle
#include <dlfcn.h> // for dladdr
#include <execinfo.h> // for backtrace
#include <fstream>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace instrumentation {
// Defined in trace.cpp: save-set / restore the per-thread in_instrumentation
// re-entrancy guard. Used by the public API entry points below via
// ScopedNoInstrument — see that struct's comment for why this is load-bearing.
NO_INSTRUMENT bool enter_no_instrument_scope();
NO_INSTRUMENT void exit_no_instrument_scope(bool prev);
} // namespace instrumentation

namespace {

// RAII: while alive, the per-thread re-entrancy guard is set, so
// __cyg_profile_func_enter no-ops. The public API entry points
// (get_call_stack(), instrumentation::resolve()) hold one for their whole
// duration: the resolver holds s_bfd_mutex while running std container/string
// template code, and under Clang the linker may pick those templates' COMDAT
// instantiations from the USER's instrumented TU — a hook firing there would
// re-lock s_bfd_mutex on the same thread (self-deadlock, observed via
// unordered_map::find inside resolve_no_unwind). Saving/restoring the previous
// value keeps the wrapper correct when the enter hook (guard already set)
// calls instrumentation::resolve(). The destructor also restores the guard if
// the wrapped code throws (get_call_stack's backtrace-failure path).
struct ScopedNoInstrument {
    bool prev;
    NO_INSTRUMENT ScopedNoInstrument() : prev(instrumentation::enter_no_instrument_scope()) {}
    NO_INSTRUMENT ~ScopedNoInstrument() { instrumentation::exit_no_instrument_scope(prev); }
    ScopedNoInstrument(const ScopedNoInstrument&) = delete;
    ScopedNoInstrument& operator=(const ScopedNoInstrument&) = delete;
};

// Takes const char* directly to avoid constructing a temporary std::string at each
// call site — all callers pass const char* from BFD/dladdr.
NO_INSTRUMENT
std::string demangle_cxa(const char* mangled) {
    int status;
    std::unique_ptr<char, void (*)(void*)> realname(
            abi::__cxa_demangle(mangled, nullptr, nullptr, &status), &free);
    if (status != 0) {
        return std::string(mangled);
    }

    return realname ? std::string(realname.get()) : "";
}

// is_std_library_symbol() — the Clang-only runtime std-library filter consulted
// by resolve_function_name() — lives in include/stdSymbolFilter.h so its pure
// mangled-name parsing is unit-testable on both compilers.

} // namespace

namespace instrumentation {

// Must be called with s_bfd_mutex already held (locked in resolve()).
bfdResolver::storedBfd* bfdResolver::ensure_bfd_loaded(Dl_info& _info) {
    // Load the corresponding bfd file (from file or map). Single map lookup on
    // the hot path: find() locates the cached entry, and on a miss the emplace's
    // iterator is reused for the return — no separate count()/at() round trips.
    auto it = bfds().find(_info.dli_fbase);
    if (it == bfds().end()) {
        // Negative cache: a previous load attempt for this object already failed.
        // Don't retry bfd_openr on every traced call — the failure is sticky for
        // the process lifetime (matching bfds(), which is never invalidated).
        if (bfd_load_failed().count(_info.dli_fbase) != 0) {
            return nullptr;
        }
        ensure_actual_executable(_info);
        // Stack local (moved into the map on success) — no reason to heap-allocate
        // a temporary that is unconditionally consumed or discarded in this scope.
        storedBfd newBfd(bfd_openr(_info.dli_fname, nullptr), &bfd_close);
        if (!newBfd.abfd) {
            bfd_load_failed().insert(_info.dli_fbase);
            return nullptr;
        }
        if (!bfd_check_format(newBfd.abfd.get(), bfd_object)) {
            bfd_load_failed().insert(_info.dli_fbase);
            return nullptr;
        }
        long storageNeeded = bfd_get_symtab_upper_bound(newBfd.abfd.get());
        if (storageNeeded < 0) {
            bfd_load_failed().insert(_info.dli_fbase);
            return nullptr;
        }
        newBfd.symbols.reset(reinterpret_cast<asymbol**>(new char[static_cast<size_t>(storageNeeded)]));
        if (bfd_canonicalize_symtab(newBfd.abfd.get(), newBfd.symbols.get()) < 0) {
            // Canonicalization failed (malformed symbol table): the buffer contents
            // are undefined and bfd_find_nearest_line() would chase garbage pointers.
            // Treat it like any other unloadable object.
            bfd_load_failed().insert(_info.dli_fbase);
            return nullptr;
        }

        newBfd.offset = reinterpret_cast<intptr_t>(_info.dli_fbase);
        it = bfds().emplace(_info.dli_fbase, std::move(newBfd)).first;
    }
    return &it->second;
}

// Must be called with s_bfd_mutex already held (locked in resolve()).
void bfdResolver::check_bfd_initialized() {
    if (!s_bfd_initialized) {
        bfd_init();
        s_bfd_initialized = true;
    }
}

std::string bfdResolver::get_argv0() {
    std::string argv0;
    std::ifstream ifs("/proc/self/cmdline");
    std::getline(ifs, argv0, '\0');
    return argv0;
}

void bfdResolver::ensure_actual_executable(Dl_info& symbol_info) {
    // Mutates symbol_info.dli_fname to be filename to open and returns filename
    // to display
    if (symbol_info.dli_fname == argv0()) {
        // dladdr returns argv[0] in dli_fname for symbols contained in
        // the main executable, which is not a valid path if the
        // executable was found by a search of the PATH environment
        // variable; In that case, we actually open /proc/self/exe, which
        // is always the actual executable (even if it was deleted/replaced!)
        // but display the path that /proc/self/exe links to.
        symbol_info.dli_fname = "/proc/self/exe";
    }
}

asection* bfdResolver::find_containing_section(storedBfd& currBfd, void* address,
                                               intptr_t& offset_out) {
    asection* section = currBfd.abfd->sections;
    // Rebasing heuristic: when the first section's VMA lies below the object's
    // load base, the object is position-independent and the raw address must
    // have the load base subtracted to become a section-relative VMA.
    const bool relative = section->vma < static_cast<uintptr_t>(currBfd.offset);
    while (section != nullptr) {
        const intptr_t offset = reinterpret_cast<intptr_t>(address)
                - (relative ? currBfd.offset : 0) - static_cast<intptr_t>(section->vma);
        if (offset >= 0 && static_cast<size_t>(offset) < section->size) {
            offset_out = offset;
            return section;
        }
        section = section->next;
    }
    return nullptr;
}

std::optional<std::string> bfdResolver::resolve_function_name(void* address) {
    Dl_info info;
    // dladdr returns 0 on failure; on failure the Dl_info contents are undefined.
    if (dladdr(address, &info) == 0 || info.dli_fbase == nullptr) {
        return "<address to object not found>";
    }
#ifndef LOG_NOT_DEMANGLED
    if (info.dli_sname == nullptr) {
        return std::nullopt;
    }
#endif

#ifdef __clang__
    // Runtime std library filter (Clang only). GCC excludes std library functions at
    // compile time via -finstrument-functions-exclude-file-list, so this is not needed.
    // See is_std_library_symbol() docstring for full explanation.
    if (info.dli_sname != nullptr && is_std_library_symbol(info.dli_sname)) {
        return std::nullopt;
    }
#endif

    storedBfd* currBfd = ensure_bfd_loaded(info);
    if (currBfd == nullptr) {
        return "<could not open object file>";
    }

    if (currBfd->abfd->sections == nullptr) {
        return "<no sections in object file>";
    }
    intptr_t offset = 0;
    asection* section = find_containing_section(*currBfd, address, offset);
    if (section == nullptr) {
        return "<not sectioned address>";
    }

    const char* file = nullptr;
    const char* func = nullptr;
    unsigned line = 0;
    if (bfd_find_nearest_line(
                currBfd->abfd.get(), section, currBfd->symbols.get(), offset, &file, &func, &line)) {
        if (func == nullptr) {
            return std::nullopt;
        }
#ifdef __clang__
        // Re-apply the std-library filter to the BFD-derived name: the dladdr
        // check above only sees dli_sname, which can be null (no dynamic symbol
        // — e.g. internal-linkage instantiations reached under LOG_NOT_DEMANGLED)
        // or a different, nearest-exported symbol than the precise symtab entry
        // BFD finds here. Either way a std-library frame could slip past the
        // first check and get logged. Cold path only: this runs once per
        // address, after which the cached nullopt makes repeats a hash hit.
        if (is_std_library_symbol(func)) {
            return std::nullopt;
        }
#endif
        auto demangled = demangle_cxa(func);
        return demangled.empty() ? std::nullopt : std::make_optional(demangled);
    }
    return demangle_cxa(info.dli_sname != nullptr ? info.dli_sname : "") + " <bfd_error>";
}

std::pair<std::string, std::optional<unsigned int>> bfdResolver::resolve_filename_and_line(void* address) {
    // Get path and offset of shared object that contains caller address.
    Dl_info info;
    // dladdr returns 0 on failure; on failure the Dl_info contents are undefined.
    if (dladdr(address, &info) == 0 || info.dli_fbase == nullptr) {
        return std::make_pair("<caller address to object not found>", std::nullopt);
    }

    storedBfd* currBfd = ensure_bfd_loaded(info);
    if (currBfd == nullptr) {
        return std::make_pair("<could not open caller object file>", std::nullopt);
    }

    if (currBfd->abfd->sections == nullptr) {
        return std::make_pair(std::string("<no sections in caller object>"), std::nullopt);
    }
    intptr_t offset = 0;
    asection* section = find_containing_section(*currBfd, address, offset);
    if (section == nullptr) {
        return std::make_pair("<not sectioned address>", std::nullopt);
    }

    const char* file = nullptr;
    const char* func = nullptr;
    unsigned int line = 0;
    if (bfd_find_nearest_line(
                currBfd->abfd.get(), section, currBfd->symbols.get(), offset, &file, &func, &line)) {
        // BFD "success" can still carry no usable location: `file` may be
        // non-null but EMPTY with line 0 (DWARF's "no source line" sentinel) —
        // observed with GCC 16 / binutils 2.46 for libc frames of optimized
        // binaries, which used to render as "(called from: :0)". Treat an empty
        // file like a null one and line 0 like an unknown line, so such frames
        // degrade through the same fallbacks as an outright lookup failure
        // (function name with ":???", then "<unknown function>").
        if (file != nullptr && file[0] != '\0') {
            return std::make_pair(std::string(file),
                                  line != 0 ? std::make_optional(line) : std::nullopt);
        }
        if (func != nullptr && func[0] != '\0') {
            return std::make_pair(demangle_cxa(func), std::nullopt);
        }
        return std::make_pair(std::string("<unknown function>"), std::nullopt);
    }
    // bfd_find_nearest_line failed for the section containing the address
    // (typical for stripped objects: no symtab, no DWARF). Degrade to the
    // <bfd_error> fallback — the address cannot be in any other section.
    // Mirrors the <bfd_error> return in resolve_function_name().
    if (info.dli_sname != nullptr) {
        return std::make_pair(demangle_cxa(info.dli_sname) + " <bfd_error>", std::nullopt);
    }
    return std::make_pair(std::string("<bfd_error>"), std::nullopt);
}

std::optional<ResolvedFrame> bfdResolver::resolve_no_unwind(
        void* callee_address, void* caller_address) {
    ResolvedFrame resolved;
    {
        // Lock covers ALL BFD operations: initialization, loading, symbol/section
        // iteration, and bfd_find_nearest_line(). BFD library is not thread-safe —
        // concurrent calls on the same bfd* object corrupt internal state. This lock
        // serializes all BFD access and also protects the name_cache() /
        // location_cache() memoization maps. Scoped so pretty_time() below — which
        // needs no BFD state — runs after the lock is released instead of extending
        // the global serialization window every traced call shares.
        std::lock_guard<std::mutex> lock(s_bfd_mutex);
        check_bfd_initialized();

        // Memoized callee-name resolution — see the cache comments in callStack.h.
        // A cached nullopt means "filtered / not loggable" and is honored as such.
        // The cached value is read in place: no optional<string> copy on a hit, and
        // on a miss the freshly resolved value moves straight into the map.
        auto name_it = name_cache().find(callee_address);
        if (name_it == name_cache().end()) {
            name_it = name_cache().emplace(callee_address,
                                           resolve_function_name(callee_address)).first;
        }
        const std::optional<std::string>& maybe_func_name = name_it->second;
        if (!maybe_func_name) {
            return std::nullopt;
        }
        resolved.callee_function_name = *maybe_func_name;

        // Memoized caller-location resolution (same call site → same file:line).
        auto loc_it = location_cache().find(caller_address);
        if (loc_it == location_cache().end()) {
            loc_it = location_cache().emplace(caller_address,
                                              resolve_filename_and_line(caller_address)).first;
        }
        resolved.caller_filename = loc_it->second.first;
        resolved.caller_line_number = loc_it->second.second;
    }

#ifdef LOG_ADDR
    resolved.callee_address = std::make_optional(callee_address);
#endif
    resolved.timestamp = utils::pretty_time();

    return std::make_optional(std::move(resolved));
}

bool bfdResolver::is_cached_filtered(void* callee_address) {
    std::lock_guard<std::mutex> lock(s_bfd_mutex);
    auto it = name_cache().find(callee_address);
    return it != name_cache().end() && !it->second;
}

std::optional<ResolvedFrame> bfdResolver::resolve(void* callee_address, void* caller_address) {
    // Fast path for callees whose cached resolution is "filtered / not loggable"
    // (every internal-linkage function on both compilers, every std-library
    // instantiation on Clang). Without it, each call to such a function still
    // paid the full _Unwind_Backtrace walk below — per call, forever — only for
    // resolve_no_unwind() to return nullopt from the name cache. The helper
    // RETURNS before unwind_nth_frame() runs, so like get_thread_fp() it is
    // never on the stack during the unwind and cannot shift the frame-6
    // constant. First sight of an address still takes the slow path (the cache
    // entry does not exist yet), which populates the cache.
    if (is_cached_filtered(callee_address)) {
        return std::nullopt;
    }
    // The caller_address passed by __cyg_profile_func_enter is the call site INSIDE the
    // instrumentation pipeline (resolve → __cyg_profile_func_enter → ...) — not the actual
    // user-code caller. Walk up the fixed depth of the pipeline to find the real one.
    //
    // If this code is not changed, walking 6 frames up from FrameUnwinder::unwind_nth_frame
    // lands at the user-code caller of the instrumented function. The chain looks like:
    //   1: FrameUnwinder::unwind_nth_frame
    //   2: instrumentation::unwind_nth_frame
    //   3: bfdResolver::resolve
    //   4: instrumentation::resolve
    //   5: __cyg_profile_func_enter
    //   6: A::foo()  — the function being instrumented (callee)
    //   7: caller of A::foo() — captured here (one beyond the 6 increments)
    //
    // If this call flow ever changes the constant must be recalculated. Only functions
    // still on the stack when _Unwind_Backtrace runs count: helpers that
    // __cyg_profile_func_enter calls BEFORE resolve() (e.g. get_thread_fp()) have
    // already returned by then and can never shift the frame numbers, inlined or not.
    //
    // Every function in frames 1-5 carries NO_INLINE (see callStack.h): without it,
    // -O2 inlines both unwind_nth_frame layers into this function (observed with
    // GCC 16 at CMAKE_BUILD_TYPE=RelWithDebInfo), the chain loses two frames, the
    // walk overshoots the real caller by two, and every line gets junk caller info.
    Callback callback(caller_address);
    unwind_nth_frame(callback, 6);
    return resolve_no_unwind(callee_address, callback.caller);
}

std::vector<std::optional<ResolvedFrame>> get_call_stack() {
    // Public API entry point — hold the re-entrancy guard for the whole capture
    // (deadlock rationale on ScopedNoInstrument). Also keeps the resolver's own
    // std internals out of the trace when the calling program is instrumented.
    ScopedNoInstrument guard;
    const size_t MAX_FRAMES = 1000;
    std::vector<void*> stack(MAX_FRAMES);
    int num = backtrace(&stack[0], MAX_FRAMES);
    if (num <= 0) {
        throw std::runtime_error("Callstack could not be built");
    }
    while (size_t(num) == stack.size()) {
        stack.resize(stack.size() * 2);
        num = backtrace(&stack[0], int(stack.size()));
    }
    stack.resize(static_cast<size_t>(num));
    std::vector<std::optional<ResolvedFrame>> res;
    res.reserve(static_cast<size_t>(num));

    // backtrace() fills stack[i] with the return address from frame i.
    // stack[0] is inside get_call_stack itself — omit it.
    // For each ancestor i (>= 1):
    //   - stack[i] is an address INSIDE frame i's function — use it as the callee
    //     for function-name resolution.
    //   - The call site INTO frame i's function lives in frame i+1's body, just
    //     before the resume point at stack[i+1]. Subtract 1 byte to point inside
    //     the call instruction (matching what the unwinder's ip_before_instruction
    //     adjustment does for the instrumentation flow).
    //   - For the outermost frame there is no parent, so caller falls back to
    //     stack[i] itself; resolve will report the function's own location.
    //
    // Use bfdResolver::resolve_no_unwind here — we have the actual addresses, no
    // need to ask the unwinder to compute them (which would land at one fixed
    // location regardless of i, producing the wrong caller info for every frame).
    const size_t n = static_cast<size_t>(num);
    for (size_t i = 1; i < n; ++i) {
        void* caller = stack[i];
        if (i + 1 < n) {
            caller = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack[i + 1]) - 1);
        }
        res.push_back(bfdResolver::resolve_no_unwind(stack[i], caller));
    }
    return res;
}

std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address) {
    // Public API entry point — same guard as get_call_stack(). When the enter
    // hook is the caller the guard is already set and this saves/restores it
    // unchanged. Frame-depth note: the guard's constructor returns before
    // bfdResolver::resolve() runs, so it is never on the stack during the
    // unwind and cannot shift the frame-6 constant.
    ScopedNoInstrument guard;
    return bfdResolver::resolve(callee_address, caller_address);
}

} // namespace instrumentation
