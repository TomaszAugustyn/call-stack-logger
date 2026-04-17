#
# Copyright © 2020-2023 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# Set include directories
include_directories("../include")
link_directories("../lib")

# Set header files
file(GLOB_RECURSE HEADERS "../include/*.h")

# Static library: compiled WITHOUT instrumentation flags. All functions in the library
# have NO_INSTRUMENT attribute, so instrumentation hooks are never needed here.
add_library(callstacklogger STATIC callStack.cpp trace.cpp ${HEADERS})
target_link_libraries(callstacklogger -ldl -lbfd)

# Demo executable: compiled WITH instrumentation (user code).
# INSTRUMENT_FLAGS is set in the root CMakeLists.txt with compiler-specific flags.
add_executable(runDemo main.cpp)
target_compile_options(runDemo PRIVATE ${INSTRUMENT_FLAGS})
target_link_libraries(runDemo callstacklogger)

# Set definition for "LOG_ADDR" option
if(LOG_ADDR)
    add_definitions(-DLOG_ADDR)
endif()

# Set definition for "LOG_NOT_DEMANGLED" option
if(LOG_NOT_DEMANGLED)
    add_definitions(-DLOG_NOT_DEMANGLED)
endif()
