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
* @brief Header file for the call-stack functionality.
*/

/*
    [Tomasz Augustyn] Changes for own usage:
    06-10-2020: 
    * change namespace names,
    * add `resolve` standalone function,
    * add `__attribute__((no_instrument_function))` to exclude from instrumentation,
	* split `bfdResolver` struct to declaration (.h) and definition (.cpp),
	* initialize static members `bfds` and `bfd_initialized` inside the class during declaration (c++17).
*/

#pragma once 

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <bfd.h>
#include <dlfcn.h>

namespace instrumentation {

	/**
	 * @brief class to load symbols and resolve address pointers
	 * @details uses binutils to interpret bfds. caches the bfd data to only read them once per run of the application
	 * for the use of dladdr see also https://sourceware.org/git/?p=glibc.git;a=blob;f=debug/backtracesyms.c
	 */
	struct bfdResolver {
		public:
			/// @brief relevant information belonging to a single bfd
			struct storedBfd {
				typedef bfd_boolean(deleter_t)(bfd*);
				std::unique_ptr<bfd, deleter_t*> abfd;
				std::unique_ptr<asymbol*[]> symbols;
				intptr_t offset;

				storedBfd(bfd *_abfd, deleter_t *_del) : abfd(_abfd, _del) {}
			};
			
			__attribute__((no_instrument_function))
			static bool ensure_bfd_loaded(Dl_info &_info);
			
			__attribute__((no_instrument_function))
			static std::pair<uintptr_t, uintptr_t> get_range_of_section(void * _addr, std::string _name);
			
			__attribute__((no_instrument_function))
			static std::string resolve(void *address);

		private:
			inline static std::map<void *, storedBfd> bfds = {};
			inline static bool bfd_initialized = false;
	};

	/**
	* @brief Returns a string representation of the current call-stack (excluding the function itself).
	* @details Per default this uses the binutils library to get the following information:
	* [address .section] filename:line (function)
	* if all of these are available. 
	*/
    __attribute__((no_instrument_function))
	std::string get_call_stack();
	
	/**
	* @brief Returns the address range of the elf-section names @a _name as part of the executable / so file that contained @a _addr.
	*/
    __attribute__((no_instrument_function))
	std::pair<uintptr_t, uintptr_t> get_range_of_section(void* _addr, std::string _name);

	/**
	* @brief Returns std::string with human-readable information about the function which pointer is passed.
	*/
    __attribute__((no_instrument_function))
    std::string resolve(void *address);
}
