/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Instrumented driver for the stripped-caller regression test — compiled WITH
 * -finstrument-functions and linked against libstripped_caller.so.
 *
 * traced_callback() is entered from a file-local function inside the stripped
 * library, so its caller address resolves to an object with no symbol and no
 * line info. resolve_filename_and_line() once looped forever on exactly this
 * input (holding the global BFD mutex); the integration test runs this program
 * under `timeout` and expects normal completion with the callback traced.
 */

#include <cstdio>

extern "C" void invoke_callback(void (*)());

void traced_callback() {
    std::puts("callback executed");
}

int main() {
    invoke_callback(&traced_callback);
    std::puts("STRIPPED_CALLER_DONE");
    return 0;
}
