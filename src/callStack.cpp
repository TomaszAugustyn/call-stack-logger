/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#include "callStack.h"
#include "backtraceExtension.h"
#include "frameReconcile.h"
#include "prettyTime.h"
#include "stdSymbolFilter.h"
#include "tracerScope.h"

#include <backtrace.h>
#include <cstdio> // for fprintf
#include <cstdlib> // for strtoull
#include <cstring>
#include <cxxabi.h> // for __cxa_demangle
#include <dlfcn.h> // for dladdr1
#include <execinfo.h> // for backtrace
#include <fstream>
#include <link.h> // for link_map
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {

// Takes const char* directly to avoid constructing a temporary std::string at each
// call site — all callers pass const char* from libbacktrace/dladdr.
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

// Copies a non-owning view into the owning ResolvedFrame the public API
// returns, stamping it with the current time. Only the std::string-returning
// entry points (get_call_stack(), instrumentation::resolve()) pay for these
// copies; the enter hook formats straight from the view.
NO_INSTRUMENT
instrumentation::ResolvedFrame own_frame(const instrumentation::ResolvedFrameView& view) {
    instrumentation::ResolvedFrame frame;
    frame.timestamp = utils::pretty_time();
    frame.callee_address = view.callee_address;
    frame.callee_function_name = *view.callee_function_name;
    frame.caller_filename = *view.caller_filename;
    frame.caller_line_number = view.caller_line_number;
    return frame;
}

// What the kernel appends, in /proc/self/maps, to the path of a mapping whose
// file has been unlinked since it was mapped.
constexpr std::string_view DELETED_MAPPING_SUFFIX = " (deleted)";

// libbacktrace's error callback. It is called for malformed or unreadable
// files at registration and, per lookup, with errnum -1 when the registered
// files carry no debug information for the address ("no debug info ..."). Both
// are ordinary outcomes for the resolver — the object is negative-cached, the
// address degrades through the documented fallback texts — so nothing is
// printed, exactly as the BFD-based resolver printed nothing on its failures.
NO_INSTRUMENT
void on_backtrace_error(void* /*data*/, const char* /*message*/, int /*errnum*/) noexcept {}

// The callbacks libbacktrace invokes from C. They copy every string before
// returning (the documentation says the buffers may not outlive the callback)
// and catch everything: the copies allocate, and an exception must not unwind
// through libbacktrace's C frames. An allocation failure leaves `failed` set,
// which look_up() turns into "no information", like an address no file knows.

struct PcInfoCollector {
    std::vector<instrumentation::addressResolver::DwarfEntry> entries;
    bool failed = false;
};

NO_INSTRUMENT
int collect_pcinfo(void* data, uintptr_t /*pc*/, const char* file, int line, const char* function) noexcept {
    auto* out = static_cast<PcInfoCollector*>(data);
    try {
        instrumentation::addressResolver::DwarfEntry entry;
        entry.function = function != nullptr ? function : "";
        entry.file = file != nullptr ? file : "";
        entry.line = line > 0 ? static_cast<unsigned int>(line) : 0u;
        out->entries.push_back(std::move(entry));
    } catch (...) {
        out->failed = true;
        return 1; // stop the walk
    }
    return 0;
}

struct SymInfoCollector {
    std::string name;
    uintptr_t value = 0;
    bool failed = false;
};

NO_INSTRUMENT
void collect_syminfo(void* data, uintptr_t /*pc*/, const char* name, uintptr_t value,
                     uintptr_t /*size*/) noexcept {
    auto* out = static_cast<SymInfoCollector*>(data);
    if (name == nullptr) {
        return; // no symbol covers the address
    }
    try {
        out->name = name;
        out->value = value;
    } catch (...) {
        out->failed = true;
    }
}

} // namespace

namespace instrumentation {

backtrace_state* addressResolver::state() {
    // Created on first use, under s_resolver_mutex (every caller holds it), and
    // never freed. A plain function-local pointer, not a magic static: a
    // failed creation stays nullptr and is retried on the next lookup.
    static backtrace_state* instance = nullptr;
    if (instance == nullptr) {
        instance = cslg_backtrace_create_state(&on_backtrace_error, nullptr);
    }
    return instance;
}

// Must be called with s_resolver_mutex already held (locked in resolve()).
addressResolver::ObjectFile* addressResolver::ensure_object_loaded(const Dl_info& _info, uintptr_t load_bias) {
    // Single map lookup on the hot path: find() locates the cached entry, and
    // on a miss the emplace's iterator is reused for the return — no separate
    // count()/at() round trips.
    auto it = objects().find(_info.dli_fbase);
    if (it == objects().end()) {
        // Negative cache: a previous registration attempt for this object
        // already failed. Don't retry the open on every traced call — the
        // failure is sticky for the process lifetime (matching objects(),
        // which is never invalidated).
        if (object_load_failed().count(_info.dli_fbase) != 0) {
            return nullptr;
        }
        // An empty path means no usable file name exists for this object (no
        // mapping found and dladdr() left dli_fname null — e.g. objects the
        // dynamic linker knows no path for). Negative-cache like any other
        // unloadable object.
        const std::string path = object_file_path(_info);
        if (path.empty()) {
            object_load_failed().insert(_info.dli_fbase);
            return nullptr;
        }
        backtrace_state* bt = state();
        if (bt == nullptr) {
            object_load_failed().insert(_info.dli_fbase);
            return nullptr;
        }
        // Registers the file's symbol table and DWARF data (or those of its
        // separate debug file) with the shared state at the object's load
        // bias: lookups then take runtime addresses as they are. A file that
        // cannot be read, is not ELF, or is truncated adds nothing and is
        // negative-cached — the open happens lazily at first sight of an
        // address inside the object, possibly long after the load, when the
        // file may have been deleted or replaced (see object_file_path).
        ObjectFile object;
        object.load_bias = load_bias;
        int has_symtab = 0;
        int has_dwarf = 0;
        int has_symtab_section = 0;
        if (cslg_backtrace_add_file(bt, path.c_str(), load_bias, &on_backtrace_error, nullptr, &has_symtab,
                                    &has_dwarf, &has_symtab_section)
            == 0) {
            object_load_failed().insert(_info.dli_fbase);
            return nullptr;
        }
        object.has_symtab = has_symtab != 0;
        object.has_dwarf = has_dwarf != 0;
        object.has_symtab_section = has_symtab_section != 0;
        // The executable itself with neither a symbol table nor debug
        // information (stripped with `strip --strip-all`, and no separate
        // debug file found through its .gnu_debuglink or build ID): no
        // function in it can be named, so its traced calls leave no lines and
        // the trace file stays empty. Say so once, on stderr like the other
        // warnings, instead of leaving an empty file to puzzle over. Shared
        // libraries without symbols are not reported: system libraries come
        // that way, and their functions are not the ones being traced.
        if (path == "/proc/self/exe" && !object.has_dwarf && !object.has_symtab_section) {
            std::fprintf(stderr,
                         "[call-stack-logger] WARNING: the executable has no symbol table and no debug "
                         "information (stripped, and no .debug file found next to it or under "
                         "/usr/lib/debug): no function in it can be named, so its calls are not traced. "
                         "Keep a .gnu_debuglink debug file with a stripped binary (see README).\n");
        }
        it = objects().emplace(_info.dli_fbase, object).first;
    }
    return &it->second;
}

std::string addressResolver::get_argv0() {
    std::string argv0;
    std::ifstream ifs("/proc/self/cmdline");
    std::getline(ifs, argv0, '\0');
    return argv0;
}

std::optional<std::string> addressResolver::mapped_object_path(void* base) {
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

std::string addressResolver::object_file_path(const Dl_info& symbol_info) {
    // The null check is load-bearing: comparing a null const char* against
    // std::string is undefined behavior.
    if (symbol_info.dli_fname != nullptr && symbol_info.dli_fname == argv0()) {
        // dladdr returns argv[0] in dli_fname for symbols contained in
        // the main executable, which is not a valid path if the
        // executable was found by a search of the PATH environment
        // variable; In that case, we actually open /proc/self/exe, which
        // is always the actual executable (even if it was deleted/replaced!)
        // but display the path that /proc/self/exe links to. (libbacktrace
        // resolves the symlink itself when it looks for a separate debug
        // file next to the executable.)
        return "/proc/self/exe";
    }
    // For every other object dli_fname is the string the dynamic linker loaded
    // it by, verbatim — for dlopen("./plugin.so") that is the relative string.
    // This runs at FIRST SIGHT of an address inside the object, which can be
    // long after the dlopen and after the program chdir()ed: the relative path
    // then names an unrelated file (symbol names and lines come from the wrong
    // object) or nothing ("<could not open object file>" for every frame).
    // The kernel records the absolute path of each file-backed mapping at mmap
    // time, so the mapping containing dli_fbase is the reliable source.
    if (std::optional<std::string> mapped = mapped_object_path(symbol_info.dli_fbase)) {
        // The kernel appends DELETED_MAPPING_SUFFIX to the path of a mapping
        // whose file was unlinked after it was mapped. No file can be trusted
        // for such an object: the old path may now hold a rebuilt or unrelated
        // file, and anyone who can write to the directory can create a file
        // under the suffixed name too. The object is negative-cached instead —
        // honest, unlike reading whatever file now sits there.
        const std::string_view path(*mapped);
        if (path.size() >= DELETED_MAPPING_SUFFIX.size()
            && path.substr(path.size() - DELETED_MAPPING_SUFFIX.size()) == DELETED_MAPPING_SUFFIX) {
            return std::string();
        }
        return *mapped;
    }
    // No file-backed mapping contains the object (the vdso, whose [vdso]
    // mapping has no file; or /proc is not mounted). dli_fname is used only
    // when it is absolute: for the vdso it is "linux-vdso.so.1", which an
    // open() would look up in the CURRENT directory, where any file of that
    // name would be read as the vdso's symbols and lines.
    if (symbol_info.dli_fname != nullptr && symbol_info.dli_fname[0] == '/') {
        return std::string(symbol_info.dli_fname);
    }
    return std::string();
}

addressResolver::AddressInfo addressResolver::query_address(void* address) {
    AddressInfo result;
    link_map* map = nullptr;
    // dladdr1 with RTLD_DL_LINKMAP is dladdr plus the object's link_map, whose
    // l_addr is the load bias libbacktrace needs (dli_fbase is the map start,
    // which differs from the bias for a non-PIE executable). The bias is read
    // here, while the loader's lock has just been released and the map is
    // certainly alive — not later under s_resolver_mutex.
    if (dladdr1(address, &result.info, reinterpret_cast<void**>(&map), RTLD_DL_LINKMAP) == 0
        || result.info.dli_fbase == nullptr) {
        return result;
    }
    result.load_bias = map != nullptr ? static_cast<uintptr_t>(map->l_addr) : 0;
    result.valid = true;
    return result;
}

addressResolver::Lookup addressResolver::look_up(const ObjectFile& object, void* address) {
    Lookup lookup;
    backtrace_state* bt = state();
    if (bt == nullptr) {
        return lookup;
    }
    const uintptr_t pc = reinterpret_cast<uintptr_t>(address);
    if (object.has_dwarf) {
        // One callback per function containing the address (several when it
        // lies inside inlined code, innermost first); with no debug
        // information for the address the callback reports null names and
        // line 0, and an error ("no debug info", errnum -1) is not a failure.
        PcInfoCollector pcinfo;
        backtrace_pcinfo(bt, pc, &collect_pcinfo, &on_backtrace_error, &pcinfo);
        if (!pcinfo.failed) {
            lookup.entries = std::move(pcinfo.entries);
        }
        // A single all-empty entry is what libbacktrace reports when nothing
        // describes the address; keep "no entries" for that.
        if (lookup.entries.size() == 1 && lookup.entries.front().function.empty()
            && lookup.entries.front().file.empty() && lookup.entries.front().line == 0) {
            lookup.entries.clear();
        }
    }
    if (object.has_symtab) {
        // The function or object symbol whose [value, value + size) contains
        // the address, from .symtab (or .dynsym when the file has no .symtab).
        SymInfoCollector syminfo;
        backtrace_syminfo(bt, pc, &collect_syminfo, &on_backtrace_error, &syminfo);
        if (!syminfo.failed) {
            lookup.symbol = std::move(syminfo.name);
            lookup.symbol_value = syminfo.value;
        }
    }
    return lookup;
}

std::string addressResolver::function_name_of(const Lookup& lookup, NameOf name_of) {
    // BFD's bfd_find_nearest_line() chose the name like this: a DWARF linkage
    // name (DW_AT_linkage_name, a mangled "_Z..." name) was final; otherwise
    // it looked the address up in the symbol table and preferred that name,
    // which for a static function, an anonymous-namespace function or a lambda
    // compiled by GCC is the mangled name with the parameter list ("_ZL14..."),
    // where DWARF only has the bare "file_static_func" — the trace shows
    // "file_static_func(int)" because of it. For languages without name
    // mangling (C) BFD took the DWARF name as it was; the symbol table then
    // either repeats it or adds a clone suffix ("foo.cold", "foo.constprop.0"),
    // which is the one case where the symbol table loses here.
    //
    // Which DWARF function enters the rule depends on what the address is:
    // the innermost of the inline chain for a callee's entry address (the
    // hooks; BFD's choice), the outermost for a return address inside a
    // physical frame (get_call_stack(); see NameOf). The symbol table's answer
    // is the same containing symbol either way.
    const bool outermost = name_of == NameOf::outermost_function;
    const std::string& dwarf = lookup.entries.empty()
            ? std::string()
            : (outermost ? lookup.entries.back().function : lookup.entries.front().function);
    if (!dwarf.empty() && dwarf.compare(0, 2, "_Z") == 0) {
        return dwarf;
    }
    if (lookup.symbol.empty()) {
        return dwarf;
    }
    if (!dwarf.empty() && lookup.symbol.compare(0, dwarf.size(), dwarf) == 0
        && (lookup.symbol.size() == dwarf.size() || lookup.symbol[dwarf.size()] == '.')) {
        return dwarf;
    }
    return lookup.symbol;
}

std::string addressResolver::innermost_base_name(const Lookup& lookup) {
    // libbfd answered with the inlined function's own DWARF name as well, but
    // only from its second lookup inside that function on: the first one
    // returned the host's symbol and the reply was then cached in its DWARF
    // tables. The enter hook of an inlined function always looked its inline
    // chain up first, so the call sites inside it got the DWARF name. Taking
    // the DWARF name every time keeps that answer without the history.
    const std::string& dwarf = lookup.entries.empty() ? std::string() : lookup.entries.front().function;
    const std::string name = dwarf.empty() ? function_name_of(lookup, NameOf::innermost_function) : dwarf;
    if (name.empty()) {
        return std::string();
    }
    return std::string(function_base_name(demangle_cxa(name.c_str())));
}

std::optional<std::string> addressResolver::resolve_function_name(void* address, const AddressInfo& address_info,
                                                                  NameOf name_of) {
    // An invalid address_info means dladdr() failed for this address (its
    // Dl_info contents are undefined then).
    if (!address_info.valid) {
        return "<address to object not found>";
    }
    const Dl_info& info = address_info.info;
    // dli_sname is null for every callee without a dynamic symbol: static
    // functions, anonymous-namespace functions, lambdas (their operator()),
    // local classes. That is NOT a reason to drop the frame — the symbol
    // table / DWARF lookup below names such functions fine, and the
    // per-address name cache makes the extra work a one-time cost per callee.
    // The frame is dropped only when nothing has a name for it (the nullopt
    // returns below).

#ifdef __clang__
    // Runtime std library filter (Clang only). GCC excludes std library functions at
    // compile time via -finstrument-functions-exclude-file-list, so this is not needed.
    // See is_std_library_symbol() docstring for full explanation.
    if (info.dli_sname != nullptr && is_std_library_symbol(info.dli_sname)) {
        return std::nullopt;
    }
#endif

    const ObjectFile* object = ensure_object_loaded(info, address_info.load_bias);
    if (object == nullptr) {
        return "<could not open object file>";
    }

    const std::string name = function_name_of(look_up(*object, address), name_of);
    if (name.empty()) {
        // Neither the debug information nor the symbol table of the file
        // names this address. With a dynamic symbol we can still name the
        // frame; without one there is no name from any source, so the frame is
        // not loggable (a bare line would say nothing).
        if (info.dli_sname == nullptr) {
            return std::nullopt;
        }
        return demangle_cxa(info.dli_sname);
    }
#ifdef __clang__
    // Re-apply the std-library filter to the resolved name: the dladdr check
    // above only sees dli_sname, which can be null (no dynamic symbol —
    // internal-linkage std instantiations and std-internal lambdas) or a
    // different, nearest-exported symbol than the precise symbol table entry
    // found here. Either way a std-library frame could slip past the first
    // check and get logged. Cold path only: this runs once per address, after
    // which the cached nullopt makes repeats a hash hit.
    if (is_std_library_symbol(name.c_str())) {
        return std::nullopt;
    }
#endif
    auto demangled = demangle_cxa(name.c_str());
    return demangled.empty() ? std::nullopt : std::make_optional(demangled);
}

addressResolver::CachedLocation addressResolver::resolve_filename_and_line(void* address,
                                                                           const AddressInfo& address_info) {
    CachedLocation location;
    // An invalid address_info means dladdr() failed for the caller address (see
    // resolve_function_name()).
    if (!address_info.valid) {
        location.file = "<caller address to object not found>";
        return location;
    }
    const Dl_info& info = address_info.info;
    // The dynamic symbol names the containing function when nothing better turns
    // up below (a stripped object); the symbol table / DWARF answer replaces it.
    if (info.dli_sname != nullptr) {
        location.function_base = std::string(function_base_name(demangle_cxa(info.dli_sname)));
    }

    const ObjectFile* object = ensure_object_loaded(info, address_info.load_bias);
    if (object == nullptr) {
        location.file = "<could not open caller object file>";
        return location;
    }

    const Lookup lookup = look_up(*object, address);
    // The innermost function containing the address, as the printed fallback
    // text names it (see function_name_of) ...
    const std::string function = function_name_of(lookup, NameOf::innermost_function);
    // ... and, for the frame reconciliation, as DWARF names it: inside an
    // inlined function the call site's direct parent is that function, which
    // the symbol table cannot name (see innermost_base_name).
    const std::string base = innermost_base_name(lookup);
    if (!base.empty()) {
        location.function_base = base;
    }
    // A DWARF position needs both a file and a line: libbacktrace reports the
    // compilation unit's file with line 0 for an address no line row covers,
    // and BFD's "success" could carry an empty file with line 0 (DWARF's "no
    // source line" sentinel, observed for libc frames of optimized binaries,
    // which used to render as "(called from: :0)"). Such frames degrade
    // through the same fallbacks as an address without debug information: the
    // function name with ":???", then "<unknown function>".
    if (!lookup.entries.empty()) {
        const DwarfEntry& innermost = lookup.entries.front();
        if (!innermost.file.empty() && innermost.line != 0) {
            location.file = innermost.file;
            location.line = innermost.line;
            return location;
        }
    }
    if (!function.empty()) {
        location.file = demangle_cxa(function.c_str());
        return location;
    }
    // Nothing in the file describes the address (typical for stripped objects:
    // no symbol table, no DWARF). The dynamic symbol, when there is one, is
    // the last source of a name.
    if (info.dli_sname != nullptr) {
        location.file = demangle_cxa(info.dli_sname);
        return location;
    }
    location.file = "<unknown function>";
    return location;
}

bool addressResolver::resolve_no_unwind(void* callee_address, void* caller_address, ResolvedFrameView& out,
                                        NameOf name_of) {
    bool have_name = false;
    bool have_location = false;
    // The hooks' names and the physical frames' names live in separate caches
    // (see frame_name_cache); the hooks take the first branch on every call.
    std::unordered_map<void*, CachedName>& names
            = name_of == NameOf::innermost_function ? name_cache() : frame_name_cache();
    {
        // Lock covers ALL libbacktrace operations: the state's creation, every
        // registration and every lookup (libbacktrace parses a compilation
        // unit's line table lazily on first use and, in threaded mode, lets
        // two threads both parse it, keeping one copy and leaking the other).
        // The lock also protects the name_cache() / location_cache()
        // memoization maps. Timestamping and formatting happen in the callers,
        // after the lock is released, so the global serialization window every
        // traced call shares stays as small as the lookups themselves.
        //
        // Warm path: both memoized lookups hit (see the cache comments in
        // callStack.h) and no symbolizer work happens at all. The cached values
        // are handed out as pointers into the cache nodes: valid for the process
        // lifetime and safe to read after the lock is dropped (node-based maps,
        // entries never erased or modified, containers leaked — see
        // ResolvedFrameView in types.h). A cached nullopt name means "filtered /
        // not loggable" and is honored as such.
        std::lock_guard<std::mutex> lock(s_resolver_mutex);
        auto name_it = names.find(callee_address);
        if (name_it != names.end()) {
            if (!name_it->second.name) {
                return false;
            }
            out.callee_function_name = &name_it->second.name.value();
            out.callee_base_name = &name_it->second.base;
            have_name = true;
        }
        auto loc_it = location_cache().find(caller_address);
        if (loc_it != location_cache().end()) {
            out.caller_filename = &loc_it->second.file;
            out.caller_line_number = loc_it->second.line;
            out.caller_function_base = &loc_it->second.function_base;
            have_location = true;
        }
    }

    if (!have_name || !have_location) {
        // Cold path: first sight of the callee and/or the call site.
        //
        // dladdr() runs OUTSIDE s_resolver_mutex, deliberately. glibc's _dl_addr
        // takes the loader lock (dl_load_lock), and dlopen() holds that same lock
        // while it runs the loaded object's constructors. The static initializer
        // of an instrumented plugin fires the enter hook on the dlopen() thread
        // with the loader lock held, and that hook takes s_resolver_mutex. A
        // resolver that called dladdr() with s_resolver_mutex held could
        // therefore deadlock against it:
        //   thread A: s_resolver_mutex held -> dladdr() waits for dl_load_lock
        //   thread B: dl_load_lock held     -> enter hook waits for s_resolver_mutex
        // (reproduced with a plugin dlopen()ed while another thread traced not-yet-
        // cached addresses; pinned by DlopenConstructorTest). dladdr() is thread-safe
        // on its own; only libbacktrace and the caches need the mutex. On failure
        // the Dl_info contents are undefined, so a failed lookup is passed on as
        // an invalid AddressInfo.
        AddressInfo callee_info;
        AddressInfo caller_info;
        if (!have_name) {
            callee_info = query_address(callee_address);
        }
        if (!have_location) {
            caller_info = query_address(caller_address);
        }

        std::lock_guard<std::mutex> lock(s_resolver_mutex);
        // Another thread may have resolved the same address while the mutex was
        // released; find-or-emplace keeps exactly one entry per address either way,
        // and on a miss the freshly resolved value moves straight into the map.
        if (!have_name) {
            auto name_it = names.find(callee_address);
            if (name_it == names.end()) {
                name_it = names.emplace(callee_address,
                                        cache_name(resolve_function_name(callee_address, callee_info, name_of)))
                                  .first;
            }
            if (!name_it->second.name) {
                return false;
            }
            out.callee_function_name = &name_it->second.name.value();
            out.callee_base_name = &name_it->second.base;
        }
        if (!have_location) {
            auto loc_it = location_cache().find(caller_address);
            if (loc_it == location_cache().end()) {
                loc_it = location_cache()
                                 .emplace(caller_address,
                                          resolve_filename_and_line(caller_address, caller_info))
                                 .first;
            }
            out.caller_filename = &loc_it->second.file;
            out.caller_line_number = loc_it->second.line;
            out.caller_function_base = &loc_it->second.function_base;
        }
    }

#ifdef LOG_ADDR
    out.callee_address = std::make_optional(callee_address);
#endif
    return true;
}

addressResolver::CachedName addressResolver::cache_name(std::optional<std::string> name) {
    CachedName cached;
    if (name) {
        cached.base = std::string(function_base_name(*name));
    }
    cached.name = std::move(name);
    return cached;
}

void addressResolver::resolve_location(void* address, ResolvedFrameView& out) {
    // Same two-phase shape as resolve_no_unwind(): the cache is consulted under
    // s_resolver_mutex, and on a miss dladdr() runs with the mutex RELEASED (it
    // takes glibc's loader lock — see the lock-order rule in resolve_no_unwind)
    // before the lookup and the insertion re-take it.
    {
        std::lock_guard<std::mutex> lock(s_resolver_mutex);
        auto loc_it = location_cache().find(address);
        if (loc_it != location_cache().end()) {
            out.caller_filename = &loc_it->second.file;
            out.caller_line_number = loc_it->second.line;
            out.caller_function_base = &loc_it->second.function_base;
            return;
        }
    }
    const AddressInfo address_info = query_address(address);
    std::lock_guard<std::mutex> lock(s_resolver_mutex);
    auto loc_it = location_cache().find(address);
    if (loc_it == location_cache().end()) {
        loc_it = location_cache().emplace(address, resolve_event_site_location(address, address_info)).first;
    }
    out.caller_filename = &loc_it->second.file;
    out.caller_line_number = loc_it->second.line;
    out.caller_function_base = &loc_it->second.function_base;
}

namespace {

// One line-table answer for an address, for comparing rows.
struct LineRow {
    std::string file;
    unsigned int line = 0;

    NO_INSTRUMENT bool same_as(const LineRow& other) const {
        return line != 0 && line == other.line && !file.empty() && file == other.file;
    }
};

// The row covering `address`: the innermost entry's position.
NO_INSTRUMENT
LineRow line_row_at(const addressResolver::Lookup& lookup) {
    LineRow row;
    if (!lookup.entries.empty()) {
        row.file = lookup.entries.front().file;
        row.line = lookup.entries.front().line;
    }
    return row;
}

// The row at `address` resolved to its OUTERMOST inline frame: when the address
// lies inside code inlined from elsewhere, the line where the enclosing
// function called it (the last entry of the chain that has a position).
NO_INSTRUMENT
LineRow outermost_line_row_at(const addressResolver::Lookup& lookup) {
    LineRow row = line_row_at(lookup);
    if (row.line == 0) {
        return row;
    }
    for (const addressResolver::DwarfEntry& entry : lookup.entries) {
        if (!entry.file.empty() && entry.line != 0) {
            row.file = entry.file;
            row.line = entry.line;
        }
    }
    return row;
}

} // namespace

addressResolver::CachedLocation addressResolver::resolve_event_site_location(void* address,
                                                                             const AddressInfo& address_info) {
    CachedLocation location = resolve_filename_and_line(address, address_info);
    if (!location.line || !address_info.valid) {
        return location;
    }
    // GCC 13 at -O2 gives the entry of a landing pad no location of its own.
    // The pad is the first thing in the function's cold partition, and the line
    // row of whatever precedes it in .text.unlikely — another function's code —
    // covers its `call __cxa_begin_catch`, so the catch site reads as some line
    // of that other function (the throw line of exc_rec for the catch in
    // exc_rec_catcher of the test driver; GCC 16 and Clang give the entry the
    // catch clause's line). The line table cannot say where a row starts, but
    // the symbol table can bound it: a row that already covers the byte before
    // the containing symbol's first byte belongs to the code before the symbol.
    // The site is then taken from the first instruction after the call — the
    // handler's first statement, resolved to its outermost inline frame — or
    // reported without a line when that one leaks too.
    const ObjectFile* object = ensure_object_loaded(address_info.info, address_info.load_bias);
    if (object == nullptr || !object->has_symtab) {
        return location;
    }
    // The containing symbol: the function (or object) symbol whose range holds
    // the address, and its first byte.
    const Lookup at_site_lookup = look_up(*object, address);
    if (at_site_lookup.symbol.empty() || at_site_lookup.symbol_value == 0) {
        return location;
    }
    const uintptr_t symbol_start = at_site_lookup.symbol_value;
    const LineRow at_site = line_row_at(at_site_lookup);
    if (!at_site.same_as(line_row_at(look_up(*object, reinterpret_cast<void*>(symbol_start))))
        || !at_site.same_as(line_row_at(look_up(*object, reinterpret_cast<void*>(symbol_start - 1))))) {
        return location;
    }
    const LineRow after_call = outermost_line_row_at(
            look_up(*object, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(address) + 1)));
    if (after_call.line == 0 || after_call.same_as(at_site)) {
        location.line.reset();
        return location;
    }
    location.file = after_call.file;
    location.line = after_call.line;
    return location;
}

std::vector<std::string> addressResolver::read_inline_chain(void* address, const AddressInfo& address_info) {
    std::vector<std::string> chain;
    if (!address_info.valid) {
        return chain;
    }
    const ObjectFile* object = ensure_object_loaded(address_info.info, address_info.load_bias);
    if (object == nullptr) {
        return chain;
    }
    const Lookup lookup = look_up(*object, address);
    // Without a DWARF location there is no DWARF inline information either: an
    // empty chain tells the caller that nothing is known, which it treats as
    // "no inlining" only where that is provably safe.
    if (lookup.entries.empty() || lookup.entries.front().file.empty() || lookup.entries.front().line == 0) {
        return chain;
    }
    std::string innermost = innermost_base_name(lookup);
    if (innermost.empty()) {
        return chain;
    }
    // The innermost function (named from DWARF, see innermost_base_name), then
    // each function it was inlined into, outward — the order libbacktrace
    // reports.
    chain.push_back(std::move(innermost));
    for (size_t i = 1; i < lookup.entries.size(); ++i) {
        if (!lookup.entries[i].function.empty()) {
            chain.emplace_back(function_base_name(demangle_cxa(lookup.entries[i].function.c_str())));
        }
    }
    return chain;
}

const std::vector<std::string>* addressResolver::resolve_inline_chain(void* address) {
    // Same two-phase shape as resolve_no_unwind(): cache under s_resolver_mutex,
    // dladdr() with the mutex released, lookup and insertion under it again.
    {
        std::lock_guard<std::mutex> lock(s_resolver_mutex);
        auto it = inline_chain_cache().find(address);
        if (it != inline_chain_cache().end()) {
            return &it->second;
        }
    }
    const AddressInfo address_info = query_address(address);
    std::lock_guard<std::mutex> lock(s_resolver_mutex);
    auto it = inline_chain_cache().find(address);
    if (it == inline_chain_cache().end()) {
        it = inline_chain_cache().emplace(address, read_inline_chain(address, address_info)).first;
    }
    return &it->second;
}

std::optional<ResolvedFrame> addressResolver::resolve_no_unwind(void* callee_address, void* caller_address) {
    ResolvedFrameView view;
    if (!resolve_no_unwind(callee_address, caller_address, view)) {
        return std::nullopt;
    }
    return std::make_optional(own_frame(view));
}

std::optional<ResolvedFrame> addressResolver::resolve_physical_frame(void* return_address, void* caller_address) {
    ResolvedFrameView view;
    // One byte back into the call the frame made: a call that ends its
    // function returns to the first byte of the next one, which would
    // otherwise name the frame. The outermost function at that byte is the
    // frame's own even when the resume point sits in inlined code.
    void* inside = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(return_address) - 1);
    if (!resolve_no_unwind(inside, caller_address, view, NameOf::outermost_function)) {
        return std::nullopt;
    }
    if (view.callee_address) {
        view.callee_address = return_address; // LOG_ADDR: report what backtrace() reported
    }
    return std::make_optional(own_frame(view));
}

bool addressResolver::resolve(void* callee_address, void* caller_address, ResolvedFrameView& out) {
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
            callee_address, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(caller_address) - 1),
            out);
}

std::vector<std::optional<ResolvedFrame>> get_call_stack() {
    // Public API entry point — hold the re-entrancy guard for the whole capture
    // (rationale on TracerScope in tracerScope.h).
    TracerScope guard;
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
    //   - stack[i] is the return address INSIDE frame i's function: the
    //     instruction after the call frame i made. resolve_physical_frame()
    //     steps one byte back into that call and names the frame after the
    //     OUTERMOST function there — the frame's own, even when the resume
    //     point sits in a helper inlined right after the call (at -O2 the
    //     innermost function at that address is often such a helper).
    //   - The call site INTO frame i's function lives in frame i+1's body, just
    //     before the resume point at stack[i+1]. Subtract 1 byte to point inside
    //     the call instruction (the same step-back addressResolver::resolve applies
    //     to the return address the enter hook receives).
    //   - For the outermost frame there is no parent, so caller falls back to
    //     stack[i] itself; resolve will report the function's own location.
    const size_t n = static_cast<size_t>(num);
    for (size_t i = 1; i < n; ++i) {
        void* caller = stack[i];
        if (i + 1 < n) {
            caller = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack[i + 1]) - 1);
        }
        res.push_back(addressResolver::resolve_physical_frame(stack[i], caller));
    }
    return res;
}

bool resolve(void* callee_address, void* caller_address, ResolvedFrameView& out) {
    // Entry point for the enter hook — same guard as get_call_stack(). The hook
    // already holds the guard, so this saves/restores it unchanged.
    TracerScope guard;
    return addressResolver::resolve(callee_address, caller_address, out);
}

std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address) {
    // Public, owning API entry point — same guard as get_call_stack(). The copy
    // into a ResolvedFrame happens outside s_resolver_mutex.
    TracerScope guard;
    ResolvedFrameView view;
    if (!addressResolver::resolve(callee_address, caller_address, view)) {
        return std::nullopt;
    }
    return std::make_optional(own_frame(view));
}

void resolve_site(void* address, ResolvedFrameView& out) {
    // Same guard as the other entry points (the interposers call this from
    // inside their own guard, so it saves/restores it unchanged).
    TracerScope guard;
    addressResolver::resolve_location(address, out);
}

std::string demangle_symbol(const char* mangled) {
    return demangle_cxa(mangled);
}

const std::vector<std::string>* inline_chain_at(void* address) {
    // Same guard as the other entry points; the hooks already hold it.
    TracerScope guard;
    return addressResolver::resolve_inline_chain(address);
}

} // namespace instrumentation
