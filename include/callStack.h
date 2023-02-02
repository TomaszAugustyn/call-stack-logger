/*
 * Copyright © 2020-2023 Tomasz Augustyn
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
#include <optional>
#include <string>
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
        typedef bfd_boolean(deleter_t)(bfd*);
        std::unique_ptr<bfd, deleter_t*> abfd;
        std::unique_ptr<asymbol*[]> symbols;
        intptr_t offset;

        storedBfd(bfd* _abfd, deleter_t* _del) : abfd(_abfd, _del) {}
    };

    NO_INSTRUMENT
    static bool ensure_bfd_loaded(Dl_info& _info);

    NO_INSTRUMENT
    static std::optional<ResolvedFrame> resolve(void* callee_address, void* caller_address);

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
    inline static bool s_bfd_initialized = false;
    inline static std::string s_argv0 = get_argv0();
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
