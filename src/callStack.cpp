// Xerus - A General Purpose Tensor Library
// Copyright (C) 2014-2017 Benjamin Huber and Sebastian Wolf.
//
// Xerus is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version.
//
// Xerus is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with Xerus. If not, see <http://www.gnu.org/licenses/>.
//
// For further information on Xerus visit https://libXerus.org
// or contact us at contact@libXerus.org.

/**
 * @file
 * @brief Implementation of the callstack functionality and respective helper class bfdResolver.
 */

/*
    [Tomasz Augustyn] Changes for own usage:
    14-03-2021:
	* get rid of `XERUS_NO_FANCY_CALLSTACK` switch,
    * change namespace names,
    * add `resolve` standalone function,
    * get rid of always false comparison: `if ((section->flags | SEC_CODE) == 0u)`,
    * move `demangle_cxa` to anonymous namespace,
	* use tenary operator at the end of `demangle_cxa` function,
    * use `NO_INSTRUMENT` macro to exclude from instrumentation,
    * add additional condition `|| s_bfds.find(info.dli_fbase) == s_bfds.end()`,
	* check `newBfd` against nullptr before dereferencing it with `!newBfd->abfd`,
	* initialize unique_ptr<storedBfd> with `std::make_unique`,
	* convert initializing with `std::pair<...>(...)` with `std::make_pair(...)`,
	* use `LOG_ADDR` compilation flag to either log or not log function address,
	* use `LOG_NOT_DEMANGLED` compilation flag to log even not demangled functions,
	* change resolved string output format,
	* extract `check_bfd_initialized` function,
	* add `ensure_actual_executable` function,
	* divide resolving into 2 parts: `resolve_function_name` and `resolve_filename_and_line`
	  as they use different addresses,
	* resolved filename and line now indicates correct place from which the function
	  is called,
	* call `unwind_nth_frame` before resolving filename and line to get proper results,
	* get rid of `get_range_of_section` function.
*/

#include "callStack.h"
#include "unwinder.h"

// workaround for deliberately incompatible bfd.h header files on some systems.
#ifndef PACKAGE
	#define PACKAGE
#endif
#ifndef PACKAGE_VERSION
	#define PACKAGE_VERSION
#endif

#include <bfd.h>
#include <dlfcn.h> // for dladdr
#include <cxxabi.h> // for __cxa_demangle
#include <execinfo.h> // for backtrace
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <memory>
#include <map>
#include <vector>
#include <iomanip>
#include <fstream>

namespace {
	NO_INSTRUMENT
	std::string demangle_cxa(const std::string& _cxa) {
		int status;
		std::unique_ptr<char, void(*)(void*)> realname(abi::__cxa_demangle(_cxa.data(), nullptr, nullptr, &status), &free);
		if (status != 0) { return _cxa; }

		return realname ? std::string(realname.get()) : "";
	}
} // namespace

namespace instrumentation {

	bool bfdResolver::ensure_bfd_loaded(Dl_info& _info) {
		// Load the corresponding bfd file (from file or map).
		if (s_bfds.count(_info.dli_fbase) == 0) {
			ensure_actual_executable(_info);
			auto newBfd = std::make_unique<storedBfd>(bfd_openr(_info.dli_fname, nullptr), &bfd_close);
			if (!newBfd || !newBfd->abfd) {
				return false;
			}
			bfd_check_format(newBfd->abfd.get(),bfd_object);
			long storageNeeded = bfd_get_symtab_upper_bound(newBfd->abfd.get());
			if (storageNeeded < 0) {
				return false;
			}
			newBfd->symbols.reset(reinterpret_cast<asymbol**>(new char[static_cast<size_t>(storageNeeded)]));
			/*size_t numSymbols = */bfd_canonicalize_symtab(newBfd->abfd.get(), newBfd->symbols.get());

			newBfd->offset = reinterpret_cast<intptr_t>(_info.dli_fbase);
			s_bfds.insert(std::make_pair(_info.dli_fbase, std::move(*newBfd)));
		}
		return true;
	}

	void bfdResolver::check_bfd_initialized() {
		if (!s_bfd_initialized) {
			bfd_init();
			s_bfd_initialized = true;
		}
	}

	std::string bfdResolver::get_argv0() {
		std::string argv0;
		std::ifstream ifs("/proc/self/cmdline");
		std::getline(ifs, argv0, '\0');
		return argv0;
	}

	void bfdResolver::ensure_actual_executable(Dl_info &symbol_info) {
		// Mutates symbol_info.dli_fname to be filename to open and returns filename
		// to display
		if (symbol_info.dli_fname == s_argv0) {
			// dladdr returns argv[0] in dli_fname for symbols contained in
			// the main executable, which is not a valid path if the
			// executable was found by a search of the PATH environment
			// variable; In that case, we actually open /proc/self/exe, which
			// is always the actual executable (even if it was deleted/replaced!)
			// but display the path that /proc/self/exe links to.
			symbol_info.dli_fname = "/proc/self/exe";
		}
	}

	std::pair<bool, std::string> bfdResolver::resolve_function_name(void *address) {
		Dl_info info;
		dladdr(address, &info);
		if (info.dli_fbase == nullptr) {
			return std::make_pair(false, "<address to object not found>");
		}
		#ifndef LOG_NOT_DEMANGLED
			if (info.dli_sname == nullptr) { return std::make_pair(false, ""); }
		#endif

		if (!ensure_bfd_loaded(info) || s_bfds.find(info.dli_fbase) == s_bfds.end()) {
			return std::make_pair(false, "<could not open object file>");
		}
		storedBfd& currBfd = s_bfds.at(info.dli_fbase);

		asection* section = currBfd.abfd->sections;
		const bool relative = section->vma < static_cast<uintptr_t>(currBfd.offset);
		// std::cout << '\n' << "sections:\n";
		while (section != nullptr) {
			const intptr_t offset = reinterpret_cast<intptr_t>(address) - (relative ? currBfd.offset : 0) - static_cast<intptr_t>(section->vma);
			// std::cout << section->name << " " << section->id << " file: " << section->filepos << " flags: " << section->flags
			//			<< " vma: " << std::hex << section->vma << " - " << std::hex << (section->vma+section->size) << std::endl;

			if (offset < 0 || static_cast<size_t>(offset) > section->size) {
				section = section->next;
				continue;
			}

			const char* file;
			const char* func;
			unsigned line;
			if (bfd_find_nearest_line(currBfd.abfd.get(), section, currBfd.symbols.get(), offset, &file, &func, &line)) {
				auto demangled = demangle_cxa(func);
				#ifdef LOG_ADDR
					if (info.dli_saddr != nullptr) {
						demangled += " +0x";
						demangled += std::to_string(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(info.dli_saddr));
					}
				#endif

				return std::make_pair(true, std::move(demangled));
			}
			return std::make_pair(true, demangle_cxa((info.dli_sname != nullptr ? info.dli_sname : "")) + " <bfd_error>");
		}
		// std::cout << " ---- sections end ------ " << std::endl;
		return std::make_pair(false, "<not sectioned address>");
	}

	std::string bfdResolver::resolve_filename_and_line(void *address) {
		// Get path and offset of shared object that contains caller address.
		Dl_info info;
		dladdr(address, &info);
		if (info.dli_fbase == nullptr) {
			return "caller address to object not found>";
		}

		if (!ensure_bfd_loaded(info) || s_bfds.find(info.dli_fbase) == s_bfds.end()) {
			return "<could not open caller object file>";
		}
		storedBfd& currBfd = s_bfds.at(info.dli_fbase);

		asection* section = currBfd.abfd->sections;
		const bool relative = section->vma < static_cast<uintptr_t>(currBfd.offset);
		// std::cout << '\n' << "sections:\n";
		while (section != nullptr) {
			const intptr_t offset = reinterpret_cast<intptr_t>(address) - (relative ? currBfd.offset : 0) - static_cast<intptr_t>(section->vma);
			// std::cout << section->name << " " << section->id << " file: " << section->filepos << " flags: " << section->flags
			//			<< " vma: " << std::hex << section->vma << " - " << std::hex << (section->vma+section->size) << std::endl;

			if (offset < 0 || static_cast<size_t>(offset) > section->size) {
				section = section->next;
				continue;
			}
			const char* file;
			const char* func;
			unsigned line = 0;
			if (bfd_find_nearest_line(currBfd.abfd.get(), section, currBfd.symbols.get(), offset, &file, &func, &line)) {
				if (file != nullptr) {
					return "(Called from: " + std::string(file) + ":" + std::to_string(line) + ")";
				}
				return "(Called from: " + demangle_cxa(func) + ")";
			}
			if (info.dli_sname != nullptr) {
				return "(Called from: " + demangle_cxa(info.dli_sname) + " <bfd_error>)";
			}
		}
		// std::cout << " ---- sections end ------ " << std::endl;
		return "(???:? <not sectioned address>)";
	}

	std::string bfdResolver::resolve(void *callee_address, void *caller_address) {
		check_bfd_initialized();
		std::stringstream res;
		#ifdef LOG_ADDR
			res << "Addr: [0x" << std::setw(int(sizeof(void*)*2)) << std::setfill('0') << std::hex << reinterpret_cast<uintptr_t>(callee_address) << "] ";
		#endif

		auto pair = resolve_function_name(callee_address);
		if (!pair.first) {
			if (pair.second.empty()) {
				return "";
			}
			return res.str() + pair.second;
		}

		// If the code is not changed 6th frame is constant as the execution flow
		// starting from 6th frame to the top of the stack will look e.g. as follows:
		// * 6th - instrumentation::FrameUnwinder::unwind_nth_frame
		// * 5th - bfdResolver::resolve instrumentation::unwind_nth_frame
		// * 4th - instrumentation::bfdResolver::resolve
		// * 3rd - instrumentation::resolve
		// * 2nd - __cyg_profile_func_enter
		// * 1st - A::foo() --> function we are interested in
		//
		// Otherwise, if this call flow is altered, frame number must be recalculated.
		Callback callback(caller_address);
		unwind_nth_frame(callback, 6);

		std::string filename = resolve_filename_and_line(callback.caller);
		return res.str() + pair.second + "  " + filename;
	}

	std::string get_call_stack() {
		const size_t MAX_FRAMES = 1000;
		std::vector<void*> stack(MAX_FRAMES);
		int num = backtrace(&stack[0], MAX_FRAMES);
		if (num <= 0) {
			return "Callstack could not be built.";
		}
		while (size_t(num) == stack.size()) {
			stack.resize(stack.size()*2);
			num = backtrace(&stack[0], int(stack.size()));
		}
		stack.resize(static_cast<size_t>(num));
		std::string res;
		// NOTE i = 0 corresponds to get_call_stack and is omitted
		for (size_t i = 1; i < static_cast<size_t>(num); ++i) {
			res += bfdResolver::resolve(stack[i], stack[i-1]) + '\n';
		}
		return res;
	}

    std::string resolve(void *callee_address, void *caller_address) {
		return bfdResolver::resolve(callee_address, caller_address);
	}

} // instrumentation