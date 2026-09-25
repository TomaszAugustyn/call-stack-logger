/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Throwing-library fixture for the LOG_EXCEPTIONS integration driver — built as
 * a shared library WITHOUT -finstrument-functions. The exception it throws is
 * born in code the tracer never saw and is caught by an instrumented frame in
 * the driver, which is exactly what an exception escaping a third-party
 * library looks like.
 */

#include <stdexcept>

extern "C" void cslg_throwing_lib_throw() {
    throw std::invalid_argument("from a library built without instrumentation");
}
