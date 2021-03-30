#pragma once

#include "types.h"
#include <algorithm>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>

namespace utils {

std::string format(const instrumentation::ResolvedFrame& frame, int current_stack_depth) {

    std::ostringstream res;
    res << "[" << frame.timestamp << "] ";

    if (frame.callee_address) {
        res << "addr: [0x" << std::setw(int(sizeof(void*) * 2)) << std::setfill('0') << std::hex
            << reinterpret_cast<uintptr_t>(*frame.callee_address) << "] ";
    }
    // Add indentation according to the current stack depth.
    if (current_stack_depth > 1) {
        std::fill_n(std::ostream_iterator<std::string>(res), current_stack_depth - 1, "|  ");
        current_stack_depth = 1;
    }
    std::fill_n(std::ostream_iterator<std::string>(res), current_stack_depth, "|_ ");

    res << frame.callee_function_name << "  (called from: " << frame.caller_filename << ":"
        << (frame.caller_line_number ? std::to_string(*frame.caller_line_number) : "???") << ")";

    return res.str();
}

} // namespace utils
