#pragma once

#include <chrono>
#include <ctime>
#include <string>

namespace utils {

// strftime format
#define LOGGER_PRETTY_TIME_FORMAT "%d-%m-%Y %H:%M:%S"

// printf format
#define LOGGER_PRETTY_MS_FORMAT ".%03ld"

// Convert current time to milliseconds since unix epoch.
template <typename T>
long to_ms(const std::chrono::time_point<T>& tp) {
    using namespace std::chrono;

    auto dur = tp.time_since_epoch();
    return duration_cast<milliseconds>(dur).count();
}

// Format it in two parts: main part with date and time and part with milliseconds.
std::string pretty_time() {
    auto tp = std::chrono::system_clock::now();
    std::time_t current_time = std::chrono::system_clock::to_time_t(tp);

    // This function use static global pointer, so it's not thread safe solution.
    std::tm* time_info = std::localtime(&current_time);

    char buffer[128];
    int string_size = strftime(buffer, sizeof(buffer), LOGGER_PRETTY_TIME_FORMAT, time_info);
    auto ms = to_ms(tp) % 1000;
    string_size +=
            std::snprintf(buffer + string_size, sizeof(buffer) - string_size, LOGGER_PRETTY_MS_FORMAT, ms);

    return std::string(buffer, buffer + string_size);
}

} // namespace utils