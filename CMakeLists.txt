#
# Copyright © 2020-2023 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

cmake_minimum_required(VERSION 3.14)

project(call-stack-logger)

option(BUILD_TESTS "Build unit and integration tests" OFF)
option(COVERAGE "Enable code coverage (requires GCC)" OFF)

# ============================================================================
# Compiler flags and instrumentation setup (project-wide)
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Base flags — no instrumentation (applied to all targets including library).
set(CMAKE_CXX_FLAGS "-g -Wall")

# -rdynamic exports all symbols to the dynamic symbol table (needed by dladdr).
add_link_options(-rdynamic)

# Code coverage support (GCC --coverage flag)
if(COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage")
endif()

# Split compilation: the callstacklogger library is compiled WITHOUT instrumentation
# (all its functions have NO_INSTRUMENT attribute). INSTRUMENT_FLAGS is applied only to
# user code targets (runDemo, traced_test_program) via target_compile_options.
#
# GCC: -finstrument-functions with exclude-file-list for std library headers.
# Clang: plain -finstrument-functions. Clang lacks the exclude-file-list, so std library
#   functions are filtered at runtime via is_std_library_symbol() in callStack.cpp.
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

    set(INSTRUMENT_FLAGS -finstrument-functions
        -finstrument-functions-exclude-file-list=${CPP_STD_INCLUDES})

elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(INSTRUMENT_FLAGS -finstrument-functions)

else()
    message(WARNING "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Function instrumentation may not work.")
    set(INSTRUMENT_FLAGS "")
endif()

# Disable instrumentation: adds the preprocessor define and clears INSTRUMENT_FLAGS
# so that no user code target receives -finstrument-functions.
# Must be set before add_subdirectory so child scopes inherit the correct values.
if(DISABLE_INSTRUMENTATION)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDISABLE_INSTRUMENTATION")
    set(INSTRUMENT_FLAGS "")
endif()

# ============================================================================
# Targets
# ============================================================================
add_subdirectory(src)

set(EXECUTABLE runDemo)

add_custom_target(run
    COMMAND ${EXECUTABLE}
)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
