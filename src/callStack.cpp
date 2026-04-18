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

namespace {

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

#ifdef __clang__
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
 */
NO_INSTRUMENT
bool is_std_library_symbol(const char* mangled_name) {
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
#endif // __clang__

} // namespace

namespace instrumentation {

// Must be called with s_bfd_mutex already held (locked in resolve()).
bool bfdResolver::ensure_bfd_loaded(Dl_info& _info) {
    // Load the corresponding bfd file (from file or map).
    if (s_bfds.count(_info.dli_fbase) == 0) {
        ensure_actual_executable(_info);
        auto newBfd = std::make_unique<storedBfd>(bfd_openr(_info.dli_fname, nullptr), &bfd_close);
        if (!newBfd || !newBfd->abfd) {
            return false;
        }
        if (!bfd_check_format(newBfd->abfd.get(), bfd_object)) {
            return false;
        }
        long storageNeeded = bfd_get_symtab_upper_bound(newBfd->abfd.get());
        if (storageNeeded < 0) {
            return false;
        }
        newBfd->symbols.reset(reinterpret_cast<asymbol**>(new char[static_cast<size_t>(storageNeeded)]));
        /*size_t numSymbols = */ bfd_canonicalize_symtab(newBfd->abfd.get(), newBfd->symbols.get());

        newBfd->offset = reinterpret_cast<intptr_t>(_info.dli_fbase);
        s_bfds.insert(std::make_pair(_info.dli_fbase, std::move(*newBfd)));
    }
    return true;
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
    if (symbol_info.dli_fname == s_argv0) {
        // dladdr returns argv[0] in dli_fname for symbols contained in
        // the main executable, which is not a valid path if the
        // executable was found by a search of the PATH environment
        // variable; In that case, we actually open /proc/self/exe, which
        // is always the actual executable (even if it was deleted/replaced!)
        // but display the path that /proc/self/exe links to.
        symbol_info.dli_fname = "/proc/self/exe";
    }
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

    if (!ensure_bfd_loaded(info)) {
        return "<could not open object file>";
    }
    storedBfd& currBfd = s_bfds.at(info.dli_fbase);

    asection* section = currBfd.abfd->sections;
    if (section == nullptr) {
        return "<no sections in object file>";
    }
    const bool relative = section->vma < static_cast<uintptr_t>(currBfd.offset);

    while (section != nullptr) {
        const intptr_t offset = reinterpret_cast<intptr_t>(address) - (relative ? currBfd.offset : 0) -
                static_cast<intptr_t>(section->vma);

        if (offset < 0 || static_cast<size_t>(offset) > section->size) {
            section = section->next;
            continue;
        }

        const char* file = nullptr;
        const char* func = nullptr;
        unsigned line = 0;
        if (bfd_find_nearest_line(
                    currBfd.abfd.get(), section, currBfd.symbols.get(), offset, &file, &func, &line)) {
            if (func == nullptr) {
                return std::nullopt;
            }
            auto demangled = demangle_cxa(func);
            return demangled.empty() ? std::nullopt : std::make_optional(demangled);
        }
        return demangle_cxa(info.dli_sname != nullptr ? info.dli_sname : "") + " <bfd_error>";
    }
    return "<not sectioned address>";
}

std::pair<std::string, std::optional<unsigned int>> bfdResolver::resolve_filename_and_line(void* address) {
    // Get path and offset of shared object that contains caller address.
    Dl_info info;
    // dladdr returns 0 on failure; on failure the Dl_info contents are undefined.
    if (dladdr(address, &info) == 0 || info.dli_fbase == nullptr) {
        return std::make_pair("<caller address to object not found>", std::nullopt);
    }

    if (!ensure_bfd_loaded(info)) {
        return std::make_pair("<could not open caller object file>", std::nullopt);
    }
    storedBfd& currBfd = s_bfds.at(info.dli_fbase);

    asection* section = currBfd.abfd->sections;
    if (section == nullptr) {
        return std::make_pair(std::string("<no sections in caller object>"), std::nullopt);
    }
    const bool relative = section->vma < static_cast<uintptr_t>(currBfd.offset);

    while (section != nullptr) {
        const intptr_t offset = reinterpret_cast<intptr_t>(address) - (relative ? currBfd.offset : 0) -
                static_cast<intptr_t>(section->vma);

        if (offset < 0 || static_cast<size_t>(offset) > section->size) {
            section = section->next;
            continue;
        }
        const char* file = nullptr;
        const char* func = nullptr;
        unsigned int line = 0;
        if (bfd_find_nearest_line(
                    currBfd.abfd.get(), section, currBfd.symbols.get(), offset, &file, &func, &line)) {
            if (file != nullptr) {
                return std::make_pair(std::string(file), std::make_optional(line));
            }
            if (func != nullptr) {
                return std::make_pair(demangle_cxa(func), std::nullopt);
            }
            return std::make_pair(std::string("<unknown function>"), std::nullopt);
        }
        if (info.dli_sname != nullptr) {
            return std::make_pair(demangle_cxa(info.dli_sname) + " <bfd_error>", std::nullopt);
        }
    }

    return std::make_pair("<not sectioned address>", std::nullopt);
}

std::optional<ResolvedFrame> bfdResolver::resolve(void* callee_address, void* caller_address) {
    // Lock covers ALL BFD operations: initialization, loading, symbol/section iteration,
    // and bfd_find_nearest_line(). BFD library is not thread-safe — concurrent calls on
    // the same bfd* object corrupt internal state. This lock serializes all BFD access.
    std::lock_guard<std::mutex> lock(s_bfd_mutex);
    check_bfd_initialized();

    auto maybe_func_name = resolve_function_name(callee_address);
    if (!maybe_func_name) {
        return std::nullopt;
    }
    ResolvedFrame resolved;
    resolved.callee_function_name = *maybe_func_name;

#ifdef LOG_ADDR
    resolved.callee_address = std::make_optional(callee_address);
#endif

    // If the code is not changed 6th frame is constant as the execution flow
    // starting from 6th frame to the top of the stack will look e.g. as follows:
    // * 6th - instrumentation::FrameUnwinder::unwind_nth_frame
    // * 5th - bfdResolver::resolve instrumentation::unwind_nth_frame
    // * 4th - instrumentation::bfdResolver::resolve
    // * 3rd - instrumentation::resolve
    // * 2nd - __cyg_profile_func_enter
    // * 1st - A::foo() --> function we are interested in
    //
    // Otherwise, if this call flow is altered, frame number must be recalculated.
    Callback callback(caller_address);
    unwind_nth_frame(callback, 6);

    auto pair = resolve_filename_and_line(callback.caller);
    resolved.caller_filename = pair.first;
    resolved.caller_line_number = pair.second;
    resolved.timestamp = utils::pretty_time();

    return std::make_optional(resolved);
}

std::vector<std::optional<ResolvedFrame>> get_call_stack() {
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
    // NOTE i = 0 corresponds to get_call_stack and is omitted
    for (size_t i = 1; i < static_cast<size_t>(num); ++i) {
        res.push_back(bfdResolver::resolve(stack[i], stack[i - 1]));
    }
    return res;
}

std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address) {
    return bfdResolver::resolve(callee_address, caller_address);
}

} // namespace instrumentation
