#pragma once

#include "types.h"
#include <iomanip>
#include <sstream>
#include <string>

namespace utils {

std::string format(const instrumentation::ResolvedFrame& frame) {
    std::stringstream res;
    res << frame.timestamp << " ";

    if (frame.callee_address) {
        res << "addr: [0x" << std::setw(int(sizeof(void*) * 2)) << std::setfill('0') << std::hex
            << reinterpret_cast<uintptr_t>(*frame.callee_address) << "] ";
    }

    res << frame.callee_function_name << "  (called from: " << frame.caller_filename << ":"
        << (frame.caller_line_number ? std::to_string(*frame.caller_line_number) : "???");

    return res.str();
}

} // namespace utils
