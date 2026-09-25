/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

// Internal to the library (LOG_EXCEPTIONS): the bridge between the exception
// interposers in exceptions.cpp, which see every throw, rethrow and catch of the
// process, and the per-thread trace state in trace.cpp, which turns them into
// event lines and reconciles the frame stack. Not installed, not part of the
// public API.

#include <typeinfo>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace instrumentation {
namespace events {

enum class ThrowKind {
    primary,       // `throw expr;` (__cxa_throw)
    rethrow,       // `throw;` inside a handler (__cxa_rethrow)
    exception_ptr, // std::rethrow_exception()
};

// Why the program is terminating (the label of the terminate line).
enum class TerminateReason {
    no_handler,        // the unwinder found no handler (throw, rethrow, rethrow_exception)
    noexcept_boundary, // the exception could not leave a noexcept function
    terminate_called,  // std::terminate() was called (by the program or by the runtime)
};

// True while the calling thread is inside the tracer (a hook, a public API
// entry point, or the exit-time windows where tracing is off). The interposers
// then forward to the runtime without logging: the tracer's own exceptions and
// handlers are not events of the traced program.
NO_INSTRUMENT
bool inside_tracer();

// Records a throw event as a child of the innermost logged frame. `type` is the
// thrown type (null when unknown), `what` its sanitized what() text or null,
// `site` an address INSIDE the throwing instruction (the interposer's return
// address minus one), resolved to file:line like a call site.
NO_INSTRUMENT
void on_throw(ThrowKind kind, const std::type_info* type, const char* what, const void* site);

// Records a catch: first reclaims the records of every frame the exception
// unwound (they lie below the catcher's level, see frameReconcile.h), marking
// their lines as left by an exception, then writes the catch line as a child
// of the catcher. `what` is the sanitized what() text or null; `dependent`
// says the caught object is a dependent exception (std::rethrow_exception),
// whose primary the interposer cannot reach and whose what() is read here
// instead. `wrapper_site` and `wrapper_frame` are the interposer's own return
// address and frame address, from which the catcher's level is derived
// (frame_level() in trace.cpp); the catch site is `wrapper_site` minus one.
// Returns true when the "catch" turned out to be the program terminating —
// the site lies in Clang's `__clang_call_terminate`, the stub its noexcept
// landing pads call — and a terminate line was written instead: the caller
// then treats the thread as terminating.
NO_INSTRUMENT
bool on_catch(const std::type_info* type, const char* what, bool dependent, const void* wrapper_site,
              const void* wrapper_frame);

// Records that the program is terminating: a "terminate" line under the
// innermost logged frame, labeled by `reason`. `type` is the exception being
// handled (null when there is none), `what` its sanitized text or null (then
// read from the current exception here), `site` an address inside the
// std::terminate() call for `terminate_called`, null otherwise. Nothing else
// is traced on the thread afterwards.
NO_INSTRUMENT
void on_terminate(TerminateReason reason, const std::type_info* type, const char* what, const void* site);

// Called once from trace_begin(): checks that the tracer's definitions of the
// five runtime entry points are the active ones in this process and can forward
// to the runtime's own. Warns on stderr when another definition is active (the
// events depending on it are then not traced); aborts with a FATAL message when
// a throw could not be forwarded at all (the C++ runtime is linked statically,
// so there is no next definition), since the program would otherwise die
// without explanation at its first throw.
NO_INSTRUMENT
void verify_interposers();

} // namespace events
} // namespace instrumentation
