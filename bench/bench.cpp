/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * The cost benchmark (see CMakeLists.txt next to this file). Every traced
 * call costs the hooks' bookkeeping plus, on the first sight of a callee or a
 * call site, the resolver's work: registering the object file with the
 * symbolizer, then the symbol and line lookups for the address. This program
 * separates the three:
 *
 *   first call into each library   bench_<lib>_first(): registers the object
 *                                  file and resolves one callee and one site;
 *   cold, per new address          bench_<lib>_run() the first time: every
 *                                  function and call site of the library is
 *                                  new (2 addresses per call);
 *   warm, per traced call          the same run repeated: every address is a
 *                                  cache hit, what remains is the hooks'
 *                                  own work and the line write.
 *
 * It prints one line per measurement (plain `key=value` pairs) plus the
 * resident memory from /proc/self/status, so run_bench.sh can tabulate
 * several builds. The trace sink is whatever CSLG_OUTPUT_FILE says; the
 * `bench` target and run_bench.sh use /dev/null.
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int bench_a_first(int x);
extern "C" int bench_b_first(int x);
extern "C" int bench_a_run(int x);
extern "C" int bench_b_run(int x);

namespace {

using clock_type = std::chrono::steady_clock;

double microseconds_between(clock_type::time_point from, clock_type::time_point to) {
    return std::chrono::duration<double, std::micro>(to - from).count();
}

// A field of /proc/self/status in kB (VmRSS: resident now, VmHWM: peak).
// C stdio on purpose: this translation unit is instrumented, and under Clang
// a std::string constructed here would instrument constexpr members of
// libstdc++ that have no out-of-line symbol and fail to link.
long status_kb(const char* key) {
    FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return -1;
    }
    char line[256];
    long value = -1;
    while (std::fgets(line, sizeof line, status) != nullptr) {
        if (std::strncmp(line, key, std::strlen(key)) == 0) {
            value = std::atol(line + std::strlen(key));
            break;
        }
    }
    std::fclose(status);
    return value;
}

} // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 50;
    const int functions = CSLG_BENCH_FUNCTIONS;
    volatile int sink = 0;

    const long rss_before_kb = status_kb("VmRSS:");

    // First traced call into each library: object registration + one callee + one site.
    const auto t0 = clock_type::now();
    sink += bench_a_first(1);
    const auto t1 = clock_type::now();
    sink += bench_b_first(1);
    const auto t2 = clock_type::now();

    // Every other function of each library, each a new callee from a new call site.
    sink += bench_a_run(1);
    const auto t3 = clock_type::now();
    sink += bench_b_run(1);
    const auto t4 = clock_type::now();

    // Warm: the same addresses again and again.
    const auto t5 = clock_type::now();
    for (int i = 0; i < iterations; ++i) {
        sink += bench_a_run(i);
        sink += bench_b_run(i);
    }
    const auto t6 = clock_type::now();

    const long rss_after_kb = status_kb("VmRSS:");
    const long peak_kb = status_kb("VmHWM:");

    // bench_<lib>_run itself is one more traced call per run; the cold pass
    // resolves (functions - 1) new leaves plus the driver, so `functions`
    // new callees and `functions` new call sites.
    const double cold_calls = 2.0 * functions;
    const double warm_calls = static_cast<double>(iterations) * 2.0 * (functions + 1);

    std::printf("functions_per_library=%d\n", functions);
    std::printf("iterations=%d\n", iterations);
    std::printf("first_call_into_library_a_us=%.1f\n", microseconds_between(t0, t1));
    std::printf("first_call_into_library_b_us=%.1f\n", microseconds_between(t1, t2));
    std::printf("cold_per_new_function_us=%.3f\n", microseconds_between(t2, t4) / cold_calls);
    std::printf("warm_per_call_ns=%.1f\n", 1000.0 * microseconds_between(t5, t6) / warm_calls);
    std::printf("rss_before_kb=%ld\n", rss_before_kb);
    std::printf("rss_after_kb=%ld\n", rss_after_kb);
    std::printf("rss_peak_kb=%ld\n", peak_kb);
    std::printf("checksum=%d\n", static_cast<int>(sink));
    return 0;
}
