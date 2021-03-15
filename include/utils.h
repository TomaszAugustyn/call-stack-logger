#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include "types.h"

namespace utils {

std::string format(const instrumentation::ResolvedFrame& frame) {
    std::stringstream res;

    if (frame.callee_address) {
		res << "Addr: [0x" << std::setw(int(sizeof(void*)*2))
            << std::setfill('0')
            << std::hex
            << reinterpret_cast<uintptr_t>(*frame.callee_address) << "] ";
    }

    res << frame.callee_function_name
        << "  (Called from: "
        << frame.caller_filename
        << ":"
        << (frame.caller_line_number ? std::to_string(*frame.caller_line_number) : "???");

    return res.str();
}

} // namespace utils
