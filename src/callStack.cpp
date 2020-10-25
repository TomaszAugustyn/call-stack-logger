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
    09-10-2020:
	* get rid of `XERUS_NO_FANCY_CALLSTACK` switch,
    * change namespace names,
    * add `resolve` standalone function,
    * get rid of always false comparison: `if ((section->flags | SEC_CODE) == 0u)`,
    * move `demangle_cxa` to anonymous namespace,
	* use tenary operator at the end of `demangle_cxa` function,
    * add `__attribute__((no_instrument_function))` to exclude from instrumentation,
    * add additional condition `|| s_bfds.find(info.dli_fbase) == s_bfds.end()`,
	* check `newBfd` against nullptr before dereferencing it with `!newBfd->abfd`,
	* initialize unique_ptr<storedBfd> with `std::make_unique`,
	* convert initializing with `std::pair<...>(...)` with `std::make_pair(...)`,
	* use `LOG_ADDR` compilation flag to either log or not log function address.
*/

#include <callStack.h>
#include <execinfo.h>

// workaround for deliberately incompatible bfd.h header files on some systems.
#ifndef PACKAGE
	#define PACKAGE
#endif
#ifndef PACKAGE_VERSION
	#define PACKAGE_VERSION
#endif

#include <bfd.h>
#include <dlfcn.h>
#include <cxxabi.h> // for __cxa_demangle
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <memory>
#include <map>
#include <vector>
#include <iomanip>

namespace {
		__attribute__((no_instrument_function))
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

	std::pair<uintptr_t, uintptr_t> bfdResolver::get_range_of_section(void* _addr, std::string _name) {
		if (!s_bfd_initialized) {
			bfd_init();
			s_bfd_initialized = true;
		}

		// Get path and offset of shared object that contains this address.
		Dl_info info;
		dladdr(_addr, &info);
		if (info.dli_fbase == nullptr) {
			return std::make_pair(0,0);
		}

		if (!ensure_bfd_loaded(info)) {
			return std::make_pair(0,0);
		}
		storedBfd &currBfd = s_bfds.at(info.dli_fbase);

		asection* section = bfd_get_section_by_name(currBfd.abfd.get(), _name.c_str());
		if (section == nullptr) {
			return std::make_pair(0,0);
		}
		return std::make_pair(section->vma, section->vma+section->size);
	}

	std::string bfdResolver::resolve(void* address) {
		if (!s_bfd_initialized) {
			bfd_init();
			s_bfd_initialized = true;
		}

		std::stringstream res;
		#ifdef LOG_ADDR
			res << "[0x" << std::setw(int(sizeof(void*)*2)) << std::setfill('0') << std::hex << reinterpret_cast<uintptr_t>(address);
		#endif

		// Get path and offset of shared object that contains this address.
		Dl_info info;
		dladdr(address, &info);
		if (info.dli_fbase == nullptr) {
			#ifdef LOG_ADDR
				res << " .?] ";
			#endif
			return res.str() + "<object to address not found>";
		}

		if (!ensure_bfd_loaded(info) || s_bfds.find(info.dli_fbase) == s_bfds.end()) {
			#ifdef LOG_ADDR
				res << " .?] ";
			#endif
			return res.str() + "<could not open object file>";
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

			#ifdef LOG_ADDR
				res << " " << section->name << "] ";
			#endif

			// Get more info on legal addresses.
			const char* file;
			const char* func;
			unsigned line;
			if (bfd_find_nearest_line(currBfd.abfd.get(), section, currBfd.symbols.get(), offset, &file, &func, &line)) {
				if (file != nullptr) {
					return res.str() + std::string(file) + ":" + std::to_string(line)+ " (inside " + demangle_cxa(func) + ")";
				}
				if (info.dli_saddr != nullptr) {
					return res.str() + "??:? (inside " + demangle_cxa(func)+ " +0x" + std::to_string(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(info.dli_saddr)) + ")";
				}
				return res.str() + "??:? (inside " + demangle_cxa(func) + ")";
			}
			return res.str() + "<bfd_error> (inside " + demangle_cxa((info.dli_sname != nullptr ? info.dli_sname : "")) + ")";
		}
		// std::cout << " ---- sections end ------ " << std::endl;
		#ifdef LOG_ADDR
			res << " .none] ";
		#endif
		return res.str() + "<not sectioned address>";
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
		// NOTE i=0 corresponds to get_call_stack and is omitted
		for (size_t i = 1; i < static_cast<size_t>(num); ++i) {
			res += bfdResolver::resolve(stack[i]) + '\n';
		}
		return res;
	}

	std::pair<uintptr_t, uintptr_t> get_range_of_section(void* _addr, std::string _name) {
		return bfdResolver::get_range_of_section(_addr, _name);
	}

    std::string resolve(void* _addr) {
		return bfdResolver::resolve(_addr);
	}

} // instrumentation