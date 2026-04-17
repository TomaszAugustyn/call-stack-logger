#
# Copyright © 2020-2023 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

cmake_minimum_required(VERSION 3.14)

# Find C++ Standard Library header files to exclude them from instrumentation
execute_process(
    COMMAND bash -c "`gcc -print-prog-name=cc1plus` -v < /dev/null 2>&1 \
    | LC_ALL=C sed -ne '/starts here/,/End of/p' \
    | grep -o '/[^\"]*' ; `gcc -print-prog-name=cpp` -v < /dev/null 2>&1 \
    | LC_ALL=C sed -ne '/starts here/,/End of/p' \
    | grep -o '/[^\"]*'"
    COMMAND bash -c "cat"
    COMMAND bash -c "sort"
    COMMAND bash -c "uniq"
    COMMAND bash -c "tr '\n' ','"
    COMMAND bash -c "sed 's/\\(.*\\),/\\1 /'"
    COMMAND bash -c "xargs"
    OUTPUT_VARIABLE CPP_STD_INCLUDES
)

string(STRIP "${CPP_STD_INCLUDES}" CPP_STD_INCLUDES)

# Exclude tracing functions themselves from instrumentation
set(CPP_STD_INCLUDES "${CPP_STD_INCLUDES},include/callStack.h,include/unwinder.h,include/types.h,include/format.h,include/prettyTime.h" CACHE STRING "")

message(STATUS "CPP_STD_INCLUDES is [${CPP_STD_INCLUDES}]")

# Set C++ compiler and flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-g -Wall -rdynamic -finstrument-functions -finstrument-functions-exclude-file-list=${CPP_STD_INCLUDES}")

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
