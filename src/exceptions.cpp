/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

// LOG_EXCEPTIONS: interposers for the C++ runtime's throw, rethrow, catch and
// terminate entry points. This translation unit is part of the static library
// that every traced executable links, so its definitions of __cxa_throw,
// __cxa_rethrow, __cxa_begin_catch, std::rethrow_exception and std::terminate
// live in the executable, and the dynamic linker resolves every call to those
// names — from the program, from libstdc++.so itself, from any shared library —
// to them first. Each one reports the event to trace.cpp and then forwards to
// the runtime's own implementation, found with dlsym(RTLD_NEXT) ("the next
// definition after the executable").
//
// The five are defined as WEAK ALIASES of privately named functions. A program
// that defines its own __cxa_throw (some backtrace-on-throw helpers do) then
// still links: its strong definition wins, ours is dropped, and
// verify_interposers() can tell because the active definition is no longer the
// private function. Weak definitions still interpose the shared runtime.
//
// This file must contain no throw and no try/catch of its own: the compiler's
// built-in declarations of these entry points, emitted for such statements,
// would clash with the definitions below. Nothing here allocates either: an
// exception raised inside __cxa_throw's wrapper would replace the program's
// exception with ours. The allocating work (symbol resolution, formatting)
// happens in trace.cpp behind its exception barriers.

#include "exceptionEvents.h"

#if defined(LOG_EXCEPTIONS) && !defined(DISABLE_INSTRUMENTATION)

    #include "eventFormat.h"
    #include <cstdint>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <cxxabi.h>
    #include <dlfcn.h>
    #include <exception>
    #include <link.h>
    #include <typeinfo>
    #include <unwind.h>

namespace {

using instrumentation::events::inside_tracer;
using instrumentation::events::on_catch;
using instrumentation::events::on_terminate;
using instrumentation::events::on_throw;
using instrumentation::events::TerminateReason;
using instrumentation::events::ThrowKind;

using cxa_throw_fn = void (*)(void*, std::type_info*, void (*)(void*));
using cxa_rethrow_fn = void (*)();
using cxa_begin_catch_fn = void* (*)(void*);
using rethrow_exception_fn = void (*)(std::exception_ptr);
using terminate_fn = void (*)();

// Itanium C++ ABI names of std::rethrow_exception(std::exception_ptr) and std::terminate().
constexpr const char* RETHROW_EXCEPTION_SYMBOL = "_ZSt17rethrow_exceptionNSt15__exception_ptr13exception_ptrE";
constexpr const char* TERMINATE_SYMBOL = "_ZSt9terminatev";

// The runtime's own definition of `symbol`: the next one after this executable
// in the dynamic linker's search order (libstdc++.so, or a sanitizer runtime's
// interceptor that forwards there itself). Null when the runtime is linked
// statically into the executable — there is no "next" then.
template <class Fn>
NO_INSTRUMENT Fn next_definition(const char* symbol) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, symbol));
}

// A handle on the C++ runtime library itself (libstdc++.so), for looking up
// its functions by address rather than the interceptor a sanitizer runtime
// may put in front of them: the library is found through __cxa_begin_catch,
// which no sanitizer intercepts, so its RTLD_NEXT definition is the runtime's
// own. Null when it cannot be found (static runtime; then the terminate
// detection below is simply off). Never closed: the runtime never unloads.
NO_INSTRUMENT
void* runtime_library() {
    void* begin_catch = dlsym(RTLD_NEXT, "__cxa_begin_catch");
    Dl_info info;
    if (begin_catch == nullptr || dladdr(begin_catch, &info) == 0 || info.dli_fname == nullptr) {
        return nullptr;
    }
    return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
}

NO_INSTRUMENT
__attribute__((noreturn)) void cannot_forward(const char* symbol) {
    std::fprintf(stderr,
                 "[call-stack-logger] FATAL: LOG_EXCEPTIONS found no C++ runtime definition of %s to "
                 "forward to. Link the shared libstdc++ (the default; -static-libstdc++ and fully "
                 "static executables are not supported with LOG_EXCEPTIONS) or build without "
                 "LOG_EXCEPTIONS.\n",
                 symbol);
    std::abort();
}

// AddressSanitizer intercepts __cxa_throw only to unpoison the stack the throw
// is about to abandon (__asan_handle_no_return). GCC links its runtime as a
// shared library, so RTLD_NEXT reaches that interceptor and the chain stays
// intact; Clang links the runtime statically INTO the executable, where our
// definition wins and the interceptor is bypassed. Calling the hook ourselves
// covers both: a weak reference that resolves only when ASan is present.
extern "C" void __asan_handle_no_return() __attribute__((weak));

NO_INSTRUMENT
void sanitizer_no_return() {
    if (&__asan_handle_no_return != nullptr) {
        __asan_handle_no_return();
    }
}

// The what() text of a thrown object of dynamic type `type`, or null when the
// type does not derive from std::exception. Asks libstdc++'s own handler-matching
// virtual whether a `catch (std::exception&)` would catch `type`: on success it
// also adjusts `object` from the thrown type to the std::exception subobject,
// which is what makes the call correct under multiple inheritance. what() is
// noexcept, so nothing here can throw.
NO_INSTRUMENT
const char* std_exception_what(const std::type_info* type, void* object) {
    #if defined(__GLIBCXX__)
    if (type == nullptr || object == nullptr) {
        return nullptr;
    }
    void* adjusted = object;
    if (!typeid(std::exception).__do_catch(type, &adjusted, 1)) {
        return nullptr;
    }
    return static_cast<const std::exception*>(adjusted)->what();
    #else
    (void)type;
    (void)object;
    return nullptr;
    #endif
}

// libstdc++ builds the class of a native C++ exception from the characters
// "GNUCC++" and a final 0 (primary) or 1 (dependent) as a 64-bit INTEGER, so on
// a little-endian machine its bytes sit reversed in memory: compare the value,
// never the text.
NO_INSTRUMENT
constexpr _Unwind_Exception_Class exception_class_of(const char (&text)[9]) {
    _Unwind_Exception_Class value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(text[i]);
    }
    return value;
}
constexpr _Unwind_Exception_Class PRIMARY_EXCEPTION_CLASS = exception_class_of("GNUCC++\0");
constexpr _Unwind_Exception_Class DEPENDENT_EXCEPTION_CLASS = exception_class_of("GNUCC++\1");

NO_INSTRUMENT
_Unwind_Exception_Class exception_class(const void* exception_object) {
    if (exception_object == nullptr) {
        return 0;
    }
    return static_cast<const _Unwind_Exception*>(exception_object)->exception_class;
}

// True when `exception_object` (the _Unwind_Exception the runtime hands to
// __cxa_begin_catch) is a native primary C++ exception, whose thrown object
// immediately follows that header (Itanium C++ ABI 2.4.2 / 2.5).
NO_INSTRUMENT
bool is_native_primary_exception(const void* exception_object) {
    return exception_class(exception_object) == PRIMARY_EXCEPTION_CLASS;
}

// True for a native dependent exception (std::rethrow_exception), which refers
// to its primary through a private layout: its what() is read in trace.cpp
// through std::current_exception() instead.
NO_INSTRUMENT
bool is_native_dependent_exception(const void* exception_object) {
    return exception_class(exception_object) == DEPENDENT_EXCEPTION_CLASS;
}

// The code range [begin, end) of one function of the C++ runtime, from its
// dynamic symbol's size; empty when the function is not found.
struct CodeRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    NO_INSTRUMENT bool contains(const void* address) const {
        const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(address);
        return a >= begin && a < end;
    }
};

NO_INSTRUMENT
CodeRange runtime_code_range(const char* symbol) {
    CodeRange range;
    static void* const library = runtime_library();
    void* function = library != nullptr ? dlsym(library, symbol) : nullptr;
    Dl_info info;
    ElfW(Sym)* elf_symbol = nullptr;
    if (function != nullptr
        && dladdr1(function, &info, reinterpret_cast<void**>(&elf_symbol), RTLD_DL_SYMENT) != 0
        && elf_symbol != nullptr && elf_symbol->st_size > 0) {
        range.begin = reinterpret_cast<std::uintptr_t>(function);
        range.end = range.begin + elf_symbol->st_size;
    }
    return range;
}

// True — with the reason — when a __cxa_begin_catch call came from the runtime
// itself giving up on the exception, which it does right before terminating
// ("terminate is a handler" in the runtime's own words): __cxa_throw,
// __cxa_rethrow and std::rethrow_exception call it when the unwinder found no
// handler; __cxa_call_terminate (the landing pad of a noexcept function, GCC 13
// and later) and __gxx_personality_v0 (a call site with no unwind information,
// which the personality routine treats the same way) when the exception could
// not leave a frame. The ranges are those of libstdc++'s own functions (a
// sanitizer's __cxa_throw interceptor is not where the runtime gives up),
// resolved once, on the first catch. Clang's noexcept landing pads call a stub
// in the executable instead of __cxa_call_terminate; trace.cpp recognizes that
// one by name (see on_catch). Everything else that ends in std::terminate() is
// caught by the std::terminate interposer below.
NO_INSTRUMENT
bool catch_means_terminate(const void* return_address, TerminateReason& reason) {
    struct GiveUpSite {
        CodeRange range;
        TerminateReason reason;
    };
    static const GiveUpSite sites[] = {
        { runtime_code_range("__cxa_throw"), TerminateReason::no_handler },
        { runtime_code_range("__cxa_rethrow"), TerminateReason::no_handler },
        { runtime_code_range(RETHROW_EXCEPTION_SYMBOL), TerminateReason::no_handler },
        { runtime_code_range("__cxa_call_terminate"), TerminateReason::noexcept_boundary },
        { runtime_code_range("__gxx_personality_v0"), TerminateReason::noexcept_boundary },
    };
    for (const GiveUpSite& site : sites) {
        if (site.range.contains(return_address)) {
            reason = site.reason;
            return true;
        }
    }
    return false;
}

// Set once this thread's program started terminating: the default terminate
// handler rethrows and catches the exception once more to print its message,
// and a custom handler may do the same, which is the handler's doing, not the
// program's flow, so no event is logged after the terminate line.
thread_local bool t_terminating = false;

// Copies what() into a one-line buffer (see utils::sanitize_what_into) and
// returns it, or null when there is no text.
NO_INSTRUMENT
const char* sanitized_what(const char* text, char (&buffer)[utils::WHAT_TEXT_CAPACITY]) {
    if (text == nullptr) {
        return nullptr;
    }
    utils::sanitize_what_into(text, buffer, sizeof(buffer));
    return buffer;
}

} // namespace

// ---- the interposers ------------------------------------------------------

extern "C" NO_INSTRUMENT __attribute__((noreturn)) void cslg_cxa_throw(void* object, std::type_info* type,
                                                                        void (*destructor)(void*)) {
    static const cxa_throw_fn real = next_definition<cxa_throw_fn>("__cxa_throw");
    if (real == nullptr) {
        cannot_forward("__cxa_throw");
    }
    if (!inside_tracer() && !t_terminating) {
        char buffer[utils::WHAT_TEXT_CAPACITY];
        const char* what = sanitized_what(std_exception_what(type, object), buffer);
        on_throw(ThrowKind::primary, type, what, static_cast<const char*>(__builtin_return_address(0)) - 1);
    }
    sanitizer_no_return();
    real(object, type, destructor);
    __builtin_unreachable();
}

extern "C" NO_INSTRUMENT __attribute__((noreturn)) void cslg_cxa_rethrow() {
    static const cxa_rethrow_fn real = next_definition<cxa_rethrow_fn>("__cxa_rethrow");
    if (real == nullptr) {
        cannot_forward("__cxa_rethrow");
    }
    if (!inside_tracer() && !t_terminating) {
        // `throw;` rethrows the exception being handled: its type is known, its
        // object is not reachable through public interfaces (the catch line
        // that follows names the what() again).
        on_throw(ThrowKind::rethrow, abi::__cxa_current_exception_type(), nullptr,
                 static_cast<const char*>(__builtin_return_address(0)) - 1);
    }
    sanitizer_no_return();
    real();
    __builtin_unreachable();
}

extern "C" NO_INSTRUMENT __attribute__((noreturn)) void cslg_rethrow_exception(std::exception_ptr pointer) {
    static const rethrow_exception_fn real = next_definition<rethrow_exception_fn>(RETHROW_EXCEPTION_SYMBOL);
    if (real == nullptr) {
        cannot_forward("std::rethrow_exception");
    }
    if (!inside_tracer() && !t_terminating) {
    #if defined(__GLIBCXX__)
        const std::type_info* type = pointer.__cxa_exception_type();
    #else
        const std::type_info* type = nullptr;
    #endif
        on_throw(ThrowKind::exception_ptr, type, nullptr,
                 static_cast<const char*>(__builtin_return_address(0)) - 1);
    }
    sanitizer_no_return();
    real(pointer);
    __builtin_unreachable();
}

extern "C" NO_INSTRUMENT void* cslg_cxa_begin_catch(void* exception_object) noexcept {
    static const cxa_begin_catch_fn real = next_definition<cxa_begin_catch_fn>("__cxa_begin_catch");
    if (real == nullptr) {
        cannot_forward("__cxa_begin_catch");
    }
    // Forward FIRST: the runtime marks the exception as caught, after which
    // __cxa_current_exception_type() names it, whatever kind of exception it is.
    void* const handler_object = real(exception_object);
    if (!inside_tracer() && !t_terminating) {
        const std::type_info* type = abi::__cxa_current_exception_type();
        char buffer[utils::WHAT_TEXT_CAPACITY];
        const char* what = nullptr;
        if (is_native_primary_exception(exception_object)) {
            void* thrown = static_cast<char*>(exception_object) + sizeof(_Unwind_Exception);
            what = sanitized_what(std_exception_what(type, thrown), buffer);
        }
        const void* return_address = __builtin_return_address(0);
        TerminateReason reason;
        if (catch_means_terminate(return_address, reason)) {
            t_terminating = true;
            on_terminate(reason, type, what, nullptr);
        } else if (on_catch(type, what, is_native_dependent_exception(exception_object), return_address,
                            __builtin_frame_address(0))) {
            t_terminating = true;
        }
    }
    return handler_object;
}

// std::terminate() itself, for every path that does not go through one of the
// give-up sites above: the program calling it (in a handler, or with no
// exception at all), a `noexcept` violation compiled by GCC before 13 or by
// Clang (whose stub the catch interposer may already have recognized — then
// t_terminating is set and nothing more is written), the runtime's own
// callers such as a std::thread whose function threw. The type comes from the
// exception being handled, if any; its what() is read in trace.cpp.
extern "C" NO_INSTRUMENT __attribute__((noreturn)) void cslg_terminate() {
    static const terminate_fn real = next_definition<terminate_fn>(TERMINATE_SYMBOL);
    if (real == nullptr) {
        cannot_forward("std::terminate");
    }
    if (!inside_tracer() && !t_terminating) {
        t_terminating = true;
        on_terminate(TerminateReason::terminate_called, abi::__cxa_current_exception_type(), nullptr,
                     static_cast<const char*>(__builtin_return_address(0)) - 1);
    }
    real();
    __builtin_unreachable();
}

// The public names, as aliases of the functions above (see the file comment).
// The three ABI entry points are redeclared inside the namespace <cxxabi.h>
// declares them in: GCC declares __cxa_throw itself at global scope (with a
// void* type_info parameter) as soon as an included header contains a throw
// expression, and Clang checks C-linkage declarations across namespaces, so
// only a declaration matching <cxxabi.h>'s own in scope and type satisfies both.
//
// The aliases are WEAK, so a program that defines its own __cxa_throw still
// links and its definition wins — except in an AddressSanitizer build. Clang
// links its ASan runtime statically into the executable, ahead of everything
// else, and that runtime defines __cxa_throw as a weak alias of its own
// interceptor; between two weak definitions the linker keeps the first, so
// the tracer's would lose and no throw would be traced. In sanitized builds
// the aliases are therefore strong (a program with its own __cxa_throw then
// fails to link with LOG_EXCEPTIONS, loudly rather than silently).
#if defined(__SANITIZE_ADDRESS__)
    #define CSLG_ALIAS_LINKAGE
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define CSLG_ALIAS_LINKAGE
    #else
        #define CSLG_ALIAS_LINKAGE weak,
    #endif
#else
    #define CSLG_ALIAS_LINKAGE weak,
#endif
namespace __cxxabiv1 {
extern "C" void __cxa_throw(void*, std::type_info*, void (*)(void*))
        __attribute__((noreturn, CSLG_ALIAS_LINKAGE alias("cslg_cxa_throw")));
extern "C" void __cxa_rethrow() __attribute__((noreturn, CSLG_ALIAS_LINKAGE alias("cslg_cxa_rethrow")));
extern "C" void* __cxa_begin_catch(void*) noexcept
        __attribute__((CSLG_ALIAS_LINKAGE alias("cslg_cxa_begin_catch")));
} // namespace __cxxabiv1
namespace std {
void rethrow_exception(exception_ptr) __attribute__((CSLG_ALIAS_LINKAGE alias("cslg_rethrow_exception")));
void terminate() noexcept __attribute__((noreturn, CSLG_ALIAS_LINKAGE alias("cslg_terminate")));
} // namespace std

// ---- startup check --------------------------------------------------------

namespace instrumentation {
namespace events {

NO_INSTRUMENT
void verify_interposers() {
    struct Entry {
        const char* symbol;
        const char* display;
        const void* ours;
    };
    const Entry entries[] = {
        { "__cxa_throw", "__cxa_throw", reinterpret_cast<const void*>(&cslg_cxa_throw) },
        { "__cxa_rethrow", "__cxa_rethrow", reinterpret_cast<const void*>(&cslg_cxa_rethrow) },
        { "__cxa_begin_catch", "__cxa_begin_catch", reinterpret_cast<const void*>(&cslg_cxa_begin_catch) },
        { RETHROW_EXCEPTION_SYMBOL, "std::rethrow_exception",
          reinterpret_cast<const void*>(&cslg_rethrow_exception) },
        { TERMINATE_SYMBOL, "std::terminate", reinterpret_cast<const void*>(&cslg_terminate) },
    };
    for (const Entry& entry : entries) {
        const void* active = dlsym(RTLD_DEFAULT, entry.symbol);
        if (active != entry.ours) {
            // Another definition won the link (the program's own, or a statically
            // linked runtime's): the events that go through it are not traced,
            // but nothing else changes.
            std::fprintf(stderr,
                         "[call-stack-logger] WARNING: %s is provided by another definition in this "
                         "program; the exception events that go through it are not traced\n",
                         entry.display);
            continue;
        }
        if (dlsym(RTLD_NEXT, entry.symbol) == nullptr) {
            cannot_forward(entry.display);
        }
    }
}

} // namespace events
} // namespace instrumentation

#endif // LOG_EXCEPTIONS && !DISABLE_INSTRUMENTATION
