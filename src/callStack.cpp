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

// Workaround for deliberately incompatible bfd.h header files on some systems.
// Same define/undef pattern as include/callStack.h (see the comment there);
// kept self-sufficient here so this TU doesn't depend on header include order.
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

#include <cstdlib> // for strtoull
#include <cxxabi.h> // for __cxa_demangle
#include <dlfcn.h> // for dladdr
#include <execinfo.h> // for backtrace
#include <fstream>
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
        // An empty path means no usable file name exists for this object (no
        // mapping found and dladdr() left dli_fname null — e.g. objects the
        // dynamic linker knows no path for). bfd_openr must not receive that;
        // negative-cache like any other unloadable object.
        const std::string path = object_file_path(_info);
        if (path.empty()) {
            bfd_load_failed().insert(_info.dli_fbase);
            return nullptr;
        }
        // Stack local (moved into the map on success) — no reason to heap-allocate
        // a temporary that is unconditionally consumed or discarded in this scope.
        storedBfd newBfd(bfd_openr(path.c_str(), nullptr), &bfd_close);
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

std::optional<std::string> bfdResolver::mapped_object_path(void* base) {
    // Each /proc/self/maps line reads "start-end perms offset dev inode [path]".
    // The path column is absent for anonymous mappings, is a bracketed
    // pseudo-name for [vdso] / [stack] and friends, and may contain spaces.
    std::ifstream maps("/proc/self/maps");
    std::string line;
    const unsigned long long target = reinterpret_cast<uintptr_t>(base);
    while (std::getline(maps, line)) {
        char* cursor = nullptr;
        const unsigned long long start = std::strtoull(line.c_str(), &cursor, 16);
        if (cursor == line.c_str() || *cursor != '-') {
            continue;
        }
        const char* range_end = cursor + 1;
        const unsigned long long end = std::strtoull(range_end, &cursor, 16);
        if (cursor == range_end || target < start || target >= end) {
            continue;
        }
        // Skip the four fixed columns (perms, offset, dev, inode) to reach the path.
        size_t pos = static_cast<size_t>(cursor - line.c_str());
        for (int column = 0; column < 4; ++column) {
            pos = line.find_first_not_of(' ', pos);
            if (pos == std::string::npos) {
                return std::nullopt;
            }
            pos = line.find(' ', pos);
            if (pos == std::string::npos) {
                return std::nullopt;
            }
        }
        pos = line.find_first_not_of(' ', pos);
        if (pos == std::string::npos || line[pos] != '/') {
            return std::nullopt;
        }
        return line.substr(pos);
    }
    return std::nullopt;
}

std::string bfdResolver::object_file_path(const Dl_info& symbol_info) {
    // The null check is load-bearing: comparing a null const char* against
    // std::string is undefined behavior.
    if (symbol_info.dli_fname != nullptr && symbol_info.dli_fname == argv0()) {
        // dladdr returns argv[0] in dli_fname for symbols contained in
        // the main executable, which is not a valid path if the
        // executable was found by a search of the PATH environment
        // variable; In that case, we actually open /proc/self/exe, which
        // is always the actual executable (even if it was deleted/replaced!)
        // but display the path that /proc/self/exe links to.
        return "/proc/self/exe";
    }
    // For every other object dli_fname is the string the dynamic linker loaded
    // it by, verbatim — for dlopen("./plugin.so") that is the relative string.
    // This runs at FIRST SIGHT of an address inside the object, which can be
    // long after the dlopen and after the program chdir()ed: the relative path
    // then names an unrelated file (symbol names and lines come from the wrong
    // object) or nothing ("<could not open object file>" for every frame).
    // The kernel records the absolute path of each file-backed mapping at mmap
    // time, so the mapping containing dli_fbase is the reliable source. A
    // mapping of a since-deleted file carries a " (deleted)" suffix; opening
    // that fails and the object is negative-cached — honest, unlike silently
    // reading whatever file now sits at the old path.
    if (std::optional<std::string> mapped = mapped_object_path(symbol_info.dli_fbase)) {
        return *mapped;
    }
    return symbol_info.dli_fname != nullptr ? std::string(symbol_info.dli_fname) : std::string();
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

std::optional<std::string> bfdResolver::resolve_function_name(void* address, const Dl_info* dl_info) {
    // A null dl_info means dladdr() failed for this address (its Dl_info
    // contents are undefined then, so resolve_no_unwind() passes nothing on).
    if (dl_info == nullptr) {
        return "<address to object not found>";
    }
    // Private copy: ensure_bfd_loaded() may redirect dli_fname to /proc/self/exe.
    Dl_info info = *dl_info;
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

std::pair<std::string, std::optional<unsigned int>> bfdResolver::resolve_filename_and_line(
        void* address, const Dl_info* dl_info) {
    // A null dl_info means dladdr() failed for the caller address (see
    // resolve_function_name()).
    if (dl_info == nullptr) {
        return std::make_pair("<caller address to object not found>", std::nullopt);
    }
    // Private copy: ensure_bfd_loaded() may redirect dli_fname to /proc/self/exe.
    Dl_info info = *dl_info;

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
    bool have_name = false;
    bool have_location = false;
    {
        // Lock covers ALL BFD operations: initialization, loading, symbol/section
        // iteration, and bfd_find_nearest_line(). BFD library is not thread-safe —
        // concurrent calls on the same bfd* object corrupt internal state. This lock
        // serializes all BFD access and also protects the name_cache() /
        // location_cache() memoization maps. Scoped so pretty_time() below — which
        // needs no BFD state — runs after the lock is released instead of extending
        // the global serialization window every traced call shares.
        //
        // Warm path: both memoized lookups hit (see the cache comments in
        // callStack.h) and no BFD work happens at all. The cached values are read
        // in place. A cached nullopt name means "filtered / not loggable" and is
        // honored as such.
        std::lock_guard<std::mutex> lock(s_bfd_mutex);
        check_bfd_initialized();
        auto name_it = name_cache().find(callee_address);
        if (name_it != name_cache().end()) {
            if (!name_it->second) {
                return std::nullopt;
            }
            resolved.callee_function_name = *name_it->second;
            have_name = true;
        }
        auto loc_it = location_cache().find(caller_address);
        if (loc_it != location_cache().end()) {
            resolved.caller_filename = loc_it->second.first;
            resolved.caller_line_number = loc_it->second.second;
            have_location = true;
        }
    }

    if (!have_name || !have_location) {
        // Cold path: first sight of the callee and/or the call site.
        //
        // dladdr() runs OUTSIDE s_bfd_mutex, deliberately. glibc's _dl_addr takes
        // the loader lock (dl_load_lock), and dlopen() holds that same lock while it
        // runs the loaded object's constructors. The static initializer of an
        // instrumented plugin fires the enter hook on the dlopen() thread with the
        // loader lock held, and that hook takes s_bfd_mutex. A resolver that called
        // dladdr() with s_bfd_mutex held could therefore deadlock against it:
        //   thread A: s_bfd_mutex held  -> dladdr() waits for dl_load_lock
        //   thread B: dl_load_lock held -> enter hook waits for s_bfd_mutex
        // (reproduced with a plugin dlopen()ed while another thread traced not-yet-
        // cached addresses; pinned by DlopenConstructorTest). dladdr() is thread-safe
        // on its own; only BFD and the caches need the mutex. On failure the Dl_info
        // contents are undefined, so a failed lookup is passed on as a null pointer.
        Dl_info callee_info {};
        Dl_info caller_info {};
        const Dl_info* callee_dl = nullptr;
        const Dl_info* caller_dl = nullptr;
        if (!have_name && dladdr(callee_address, &callee_info) != 0
            && callee_info.dli_fbase != nullptr) {
            callee_dl = &callee_info;
        }
        if (!have_location && dladdr(caller_address, &caller_info) != 0
            && caller_info.dli_fbase != nullptr) {
            caller_dl = &caller_info;
        }

        std::lock_guard<std::mutex> lock(s_bfd_mutex);
        // Another thread may have resolved the same address while the mutex was
        // released; find-or-emplace keeps exactly one entry per address either way,
        // and on a miss the freshly resolved value moves straight into the map.
        if (!have_name) {
            auto name_it = name_cache().find(callee_address);
            if (name_it == name_cache().end()) {
                name_it = name_cache()
                                  .emplace(callee_address,
                                           resolve_function_name(callee_address, callee_dl))
                                  .first;
            }
            if (!name_it->second) {
                return std::nullopt;
            }
            resolved.callee_function_name = *name_it->second;
        }
        if (!have_location) {
            auto loc_it = location_cache().find(caller_address);
            if (loc_it == location_cache().end()) {
                loc_it = location_cache()
                                 .emplace(caller_address,
                                          resolve_filename_and_line(caller_address, caller_dl))
                                 .first;
            }
            resolved.caller_filename = loc_it->second.first;
            resolved.caller_line_number = loc_it->second.second;
        }
    }

#ifdef LOG_ADDR
    resolved.callee_address = std::make_optional(callee_address);
#endif
    resolved.timestamp = utils::pretty_time();

    return std::make_optional(std::move(resolved));
}

std::optional<ResolvedFrame> bfdResolver::resolve(void* callee_address, void* caller_address) {
    // The hook's `caller` argument is NOT the address of the call into
    // __cyg_profile_func_enter: both GCC and Clang emit the hook call as
    // `__cyg_profile_func_enter(fn, __builtin_return_address(0))` inside the
    // instrumented function, so it is that function's own return address — the
    // instruction right after the call site in the real caller. Step one byte
    // back so the address lies inside the call instruction itself; otherwise a
    // call that ends a source line would be attributed to the next line (the
    // same adjustment get_call_stack() applies to backtrace()'s return
    // addresses). No stack walk is needed to reach the caller.
    //
    // When the optimizer inlines an instrumented function into its caller, the
    // hooks still fire but __builtin_return_address(0) then belongs to the
    // enclosing physical frame, so the reported call site is that frame's — one
    // level up. Documented in README's RelWithDebInfo tip.
    return resolve_no_unwind(
            callee_address, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(caller_address) - 1));
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
    //     the call instruction (the same step-back bfdResolver::resolve applies
    //     to the return address the enter hook receives).
    //   - For the outermost frame there is no parent, so caller falls back to
    //     stack[i] itself; resolve will report the function's own location.
    //
    // Use bfdResolver::resolve_no_unwind here: the step-back is already applied
    // above, so resolve() would move the address one byte too far.
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
    // unchanged.
    ScopedNoInstrument guard;
    return bfdResolver::resolve(callee_address, caller_address);
}

} // namespace instrumentation
