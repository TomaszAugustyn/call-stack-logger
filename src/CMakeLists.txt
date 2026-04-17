#
# Copyright © 2020-2023 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

cmake_minimum_required(VERSION 3.14)

# Set C++ compiler and flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler-specific instrumentation flags.
# GCC: uses -finstrument-functions with an exclude-file-list to prevent std library
#   functions from being instrumented. The exclude list is auto-discovered from the
#   compiler's include search paths.
# Clang: uses -finstrument-functions-after-inlining (Clang does not support
#   -finstrument-functions-exclude-file-list). This only instruments functions that
#   survive inlining, naturally excluding most std library templates. A re-entrancy
#   guard in __cyg_profile_func_enter handles any remaining edge cases.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Discover C++ standard library header paths to exclude from instrumentation.
    execute_process(
        COMMAND bash -c "${CMAKE_CXX_COMPILER} -xc++ -E -v - < /dev/null 2>&1 \
        | LC_ALL=C sed -ne '/starts here/,/End of/p' \
        | grep -o '/[^\"]*'"
        COMMAND bash -c "sort"
        COMMAND bash -c "uniq"
        COMMAND bash -c "tr '\n' ','"
        COMMAND bash -c "sed 's/\\(.*\\),/\\1 /'"
        COMMAND bash -c "xargs"
        OUTPUT_VARIABLE CPP_STD_INCLUDES
    )
    string(STRIP "${CPP_STD_INCLUDES}" CPP_STD_INCLUDES)

    # Also exclude the project's instrumentation headers from instrumentation.
    set(CPP_STD_INCLUDES "${CPP_STD_INCLUDES},include/callStack.h,include/unwinder.h,include/types.h,include/format.h,include/prettyTime.h" CACHE STRING "")
    message(STATUS "CPP_STD_INCLUDES is [${CPP_STD_INCLUDES}]")

    set(CMAKE_CXX_FLAGS "-g -Wall -rdynamic -finstrument-functions -finstrument-functions-exclude-file-list=${CPP_STD_INCLUDES}")

elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang: -finstrument-functions-after-inlining instruments functions after the inlining
    # pass. Plain -finstrument-functions cannot be used because it forces out-of-line
    # versions of constexpr std library functions (e.g., basic_string::_M_init_local_buf)
    # that have no symbol in libstdc++.so, causing linker errors.
    # Clang lacks -finstrument-functions-exclude-file-list, so std library functions that
    # survive inlining are filtered at runtime in resolve_function_name() instead.
    # -rdynamic goes to link flags (Clang warns about it as a compile flag).
    set(CMAKE_CXX_FLAGS "-g -Wall -finstrument-functions-after-inlining")
    add_link_options(-rdynamic)

else()
    message(WARNING "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Function instrumentation may not work.")
    set(CMAKE_CXX_FLAGS "-g -Wall -rdynamic")
endif()

# Set include directories
include_directories("../include")
link_directories("../lib")

# Code coverage support (GCC --coverage flag)
if(COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage")
endif()

# Set header files
file(GLOB_RECURSE HEADERS "../include/*.h")

# Static library from instrumentation source files.
# All functions in these files have NO_INSTRUMENT attribute, so they are excluded
# from instrumentation even though -finstrument-functions is applied.
add_library(callstacklogger STATIC callStack.cpp trace.cpp ${HEADERS})
target_link_libraries(callstacklogger -ldl -lbfd)

# Demo executable links against the library
add_executable(runDemo main.cpp)
target_link_libraries(runDemo callstacklogger)

# Set definition for "LOG_ADDR" option
if(LOG_ADDR)
    add_definitions(-DLOG_ADDR)
endif()

# Set definition for "LOG_NOT_DEMANGLED" option
if(LOG_NOT_DEMANGLED)
    add_definitions(-DLOG_NOT_DEMANGLED)
endif()

# Disable instrumentation if necessary
if(DISABLE_INSTRUMENTATION)
    set(CMAKE_CXX_FLAGS "-g -Wall -rdynamic -DDISABLE_INSTRUMENTATION")
endif()
