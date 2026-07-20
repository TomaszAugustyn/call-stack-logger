/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Exit-window safety driver — compiled WITH -finstrument-functions.
 *
 * A global object's destructor runs during exit() processing and calls an
 * instrumented function. Under normal link order the object is constructed
 * BEFORE trace_begin() (its constructor's hooks fire while trace_ready is
 * still false) and destroyed AFTER trace_shutdown() (exit handlers run in
 * reverse registration order; the destructor's hooks find the instrumentation
 * disabled). Both windows must be silent no-ops — never a crash, hang, or
 * use-after-free. The resolver caches are deliberately leaked (callStack.h)
 * precisely so no exit-time destruction of resolver state exists for these
 * hooks to race; running this program under the ASan/TSan CI jobs pins that
 * end-to-end.
 *
 * Prints GLOBAL_DTOR_RAN from the destructor so the test can verify the
 * destructor actually executed and completed.
 *
 * Functions are deliberately NOT static — file-local linkage would hide them
 * from dladdr (the anonymous-namespace struct's destructor is internal-linkage
 * on purpose: its hook still fires, exercising the filtered path too).
 */

#include <iostream>

int traced_from_global_dtor(int x) {
    return x + 1;
}

void traced_marker_in_main() {
    std::cout << "MARKER_IN_MAIN\n";
}

namespace {
struct GlobalWithInstrumentedDtor {
    ~GlobalWithInstrumentedDtor() {
        (void)traced_from_global_dtor(7);
        std::cout << "GLOBAL_DTOR_RAN" << std::endl;
    }
};
GlobalWithInstrumentedDtor g_instrumented_dtor_object;
} // namespace

int main() {
    traced_marker_in_main();
    return 0;
}
