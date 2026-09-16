/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace utils {

// strftime format
#define LOGGER_PRETTY_TIME_FORMAT "%d-%m-%Y %H:%M:%S"

// printf format
#define LOGGER_PRETTY_MS_FORMAT ".%03ld"

// Guaranteed output length of pretty_time(): "DD-MM-YYYY HH:MM:SS.mmm" = 23 chars.
// Tied to LOGGER_PRETTY_TIME_FORMAT + LOGGER_PRETTY_MS_FORMAT. If either format
// changes, update this constant — the PrettyTimeTest.LengthMatchesConstant unit
// test enforces consistency and will fail fast on drift. Downstream code
// (notably LOG_ELAPSED in trace.cpp) derives byte offsets from this value.
inline constexpr std::size_t PRETTY_TIME_LENGTH = 23;

// Convert current time to milliseconds since unix epoch. Returns int64_t, not
// long: milliseconds since 1970 (~1.7e12) overflow a 32-bit long, so `long`
// would truncate on 32-bit Linux targets (e.g. ARM32) and corrupt the .mmm
// field. On LP64 the codegen is identical.
// NO_INSTRUMENT: called from the instrumentation pipeline (enter hook -> pretty_time_into -> to_ms).
template <typename T>
NO_INSTRUMENT
std::int64_t to_ms(const std::chrono::time_point<T>& tp) {
    using namespace std::chrono;

    auto dur = tp.time_since_epoch();
    return duration_cast<milliseconds>(dur).count();
}

// Capacity a caller-provided buffer needs for pretty_time_into(): the format
// yields PRETTY_TIME_LENGTH characters plus the terminating NUL, with generous
// headroom so a longer future format degrades to truncation, never overflow.
inline constexpr std::size_t PRETTY_TIME_BUF_SIZE = 64;

// Renders the current time as "DD-MM-YYYY HH:MM:SS.mmm" into `out` (capacity
// `cap`, NUL-terminated, truncated if it does not fit) and returns the number
// of characters written, excluding the NUL. This is the allocation-free core
// used by the enter hook with a stack buffer; pretty_time() wraps it for
// callers that want a std::string. Formatted in two parts: date and time via
// strftime, then the milliseconds via snprintf.
// NO_INSTRUMENT: called from the instrumentation pipeline.
NO_INSTRUMENT
inline std::size_t pretty_time_into(char* out, std::size_t cap) {
    if (cap == 0) {
        return 0;
    }
    auto tp = std::chrono::system_clock::now();
    std::time_t current_time = std::chrono::system_clock::to_time_t(tp);

    // The rendered "DD-MM-YYYY HH:MM:SS" prefix only changes once per second,
    // while a traced program calls pretty_time_into() for every logged line.
    // Cache the prefix per thread and re-run localtime_r + strftime only when
    // the second rolls over; the millisecond suffix is appended fresh every
    // call. thread_local keeps the cache race-free with zero synchronization.
    // A DST or timezone-offset change cannot serve a stale prefix: the UTC
    // offset shifts on a whole-second boundary, so `current_time` changes with
    // it and the cache misses. localtime_r (POSIX thread-safe variant) is used
    // instead of std::localtime, which shares a static global buffer.
    thread_local std::time_t cached_second = 0;
    thread_local std::size_t cached_size = 0;
    thread_local char cached_prefix[PRETTY_TIME_BUF_SIZE];
    if (current_time != cached_second || cached_size == 0) {
        std::tm time_info_buf{};
        localtime_r(&current_time, &time_info_buf);
        cached_size = strftime(cached_prefix, sizeof(cached_prefix), LOGGER_PRETTY_TIME_FORMAT, &time_info_buf);
        cached_second = current_time;
    }

    std::size_t size = cached_size < cap - 1 ? cached_size : cap - 1;
    std::memcpy(out, cached_prefix, size);
    // % 1000 keeps the value in [0, 999], so the narrowing cast to long (the
    // type LOGGER_PRETTY_MS_FORMAT's %03ld expects) is always lossless.
    auto ms = static_cast<long>(to_ms(tp) % 1000);
    int ms_size = std::snprintf(out + size, cap - size, LOGGER_PRETTY_MS_FORMAT, ms);
    if (ms_size > 0) {
        // snprintf reports the length it wanted; clamp to what actually fit.
        const std::size_t wanted = static_cast<std::size_t>(ms_size);
        size += wanted < cap - size ? wanted : cap - size - 1;
    }
    out[size] = '\0';
    return size;
}

// std::string-returning convenience wrapper around pretty_time_into(). Cold
// paths only (run-separator header, the public ResolvedFrame API); the enter
// hook uses pretty_time_into() directly to stay allocation-free.
// NO_INSTRUMENT: called from the instrumentation pipeline.
NO_INSTRUMENT
inline std::string pretty_time() {
    char buffer[PRETTY_TIME_BUF_SIZE];
    const std::size_t size = pretty_time_into(buffer, sizeof(buffer));
    return std::string(buffer, buffer + size);
}

} // namespace utils
