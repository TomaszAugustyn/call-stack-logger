#pragma once

#include <string>
#include <optional>

namespace instrumentation {

struct ResolvedFrame {
	std::optional<void*> callee_address;
	std::string callee_function_name;
	std::string caller_filename;
	std::optional<unsigned int> caller_line_number;
	// TODO: Add timestamp
};

} // namespace instrumentation
