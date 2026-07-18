/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Simulates a stripped third-party shared library that invokes a callback in
 * instrumented user code. Built WITHOUT instrumentation and stripped of its
 * .symtab and debug info post-build (see CMakeLists.txt), so the callback's
 * caller address lands in an object where dladdr() finds no symbol (the call
 * site is in a file-local function not covered by .dynsym) and
 * bfd_find_nearest_line() fails. Regression fixture for the
 * resolve_filename_and_line() infinite-loop fix — see StrippedCallerTest in
 * test_integration.cpp.
 */

using callback_t = void (*)();

// File-local: absent from .dynsym, so once .symtab is stripped dladdr() cannot
// attribute an address inside this function to any symbol. noinline plus the
// asm barrier keep the call to `cb` a genuine (non-tail) call, so the return
// address recorded for the callback's caller stays inside this function.
__attribute__((noinline)) static void hidden_invoke(callback_t cb) {
    cb();
    __asm__ volatile("");
}

extern "C" void invoke_callback(callback_t cb) {
    hidden_invoke(cb);
    __asm__ volatile("");
}
