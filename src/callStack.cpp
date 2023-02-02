/*
 * Copyright © 2020-2023 Tomasz Augustyn
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
#include <map>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace {

NO_INSTRUMENT
std::string demangle_cxa(const std::string& _cxa) {
    int status;
    std::unique_ptr<char, void (*)(void*)> realname(
            abi::__cxa_demangle(_cxa.data(), nullptr, nullptr, &status), &free);
    if (status != 0) {
        return _cxa;
    }

    return realname ? std::string(realname.get()) : "";
}

} // namespace

namespace instrumentation {

bool bfdResolver::ensure_bfd_loaded(Dl_info& _info) {
    // Load the corresponding bfd file (from file or map).
    if (s_bfds.count(_info.dli_fbase) == 0) {
        ensure_actual_executable(_info);
        auto newBfd = std::make_unique<storedBfd>(bfd_openr(_info.dli_fname, nullptr), &bfd_close);
        if (!newBfd || !newBfd->abfd) {
            return false;
        }
        bfd_check_format(newBfd->abfd.get(), bfd_object);
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
    dladdr(address, &info);
    if (info.dli_fbase == nullptr) {
        return "<address to object not found>";
    }
#ifndef LOG_NOT_DEMANGLED
    if (info.dli_sname == nullptr) {
        return std::nullopt;
    }
#endif

    if (!ensure_bfd_loaded(info)) {
        return "<could not open object file>";
    }
    storedBfd& currBfd = s_bfds.at(info.dli_fbase);

    asection* section = currBfd.abfd->sections;
    const bool relative = section->vma < static_cast<uintptr_t>(currBfd.offset);

    while (section != nullptr) {
        const intptr_t offset = reinterpret_cast<intptr_t>(address) - (relative ? currBfd.offset : 0) -
                static_cast<intptr_t>(section->vma);

        if (offset < 0 || static_cast<size_t>(offset) > section->size) {
            section = section->next;
            continue;
        }

        const char* file;
        const char* func;
        unsigned line;
        if (bfd_find_nearest_line(
                    currBfd.abfd.get(), section, currBfd.symbols.get(), offset, &file, &func, &line)) {
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
    dladdr(address, &info);
    if (info.dli_fbase == nullptr) {
        return std::make_pair("<caller address to object not found>", std::nullopt);
    }

    if (!ensure_bfd_loaded(info)) {
        return std::make_pair("<could not open caller object file>", std::nullopt);
    }
    storedBfd& currBfd = s_bfds.at(info.dli_fbase);

    asection* section = currBfd.abfd->sections;
    const bool relative = section->vma < static_cast<uintptr_t>(currBfd.offset);

    while (section != nullptr) {
        const intptr_t offset = reinterpret_cast<intptr_t>(address) - (relative ? currBfd.offset : 0) -
                static_cast<intptr_t>(section->vma);

        if (offset < 0 || static_cast<size_t>(offset) > section->size) {
            section = section->next;
            continue;
        }
        const char* file;
        const char* func;
        unsigned int line = 0;
        if (bfd_find_nearest_line(
                    currBfd.abfd.get(), section, currBfd.symbols.get(), offset, &file, &func, &line)) {
            if (file != nullptr) {
                return std::make_pair(std::string(file), std::make_optional(line));
            }
            return std::make_pair(demangle_cxa(func), std::nullopt);
        }
        if (info.dli_sname != nullptr) {
            return std::make_pair(demangle_cxa(info.dli_sname) + " <bfd_error>", std::nullopt);
        }
    }

    return std::make_pair("<not sectioned address>", std::nullopt);
}

std::optional<ResolvedFrame> bfdResolver::resolve(void* callee_address, void* caller_address) {
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
