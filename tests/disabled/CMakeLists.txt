#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# A consumer project, configured the way a user's project is, that proves
# DISABLE_INSTRUMENTATION=ON makes callstacklogger::instrumented inert: the
# same program is built once linking the target and once without any mention
# of the library, and compare_binaries.sh requires the two executables to
# agree on their NEEDED libraries, their debug sections and their exported
# dynamic symbols. A third program, api_program, links the plain
# callstacklogger::callstacklogger target and must still resolve its call
# stack through get_call_stack(): the on-demand API keeps working with the
# hooks compiled out, it only has to be linked explicitly. ctest runs both
# checks (run_checks.sh) through `ctest --build-and-test` from
# tests/CMakeLists.txt when the option is on (CSLG_SOURCE_DIR and the
# compiler come from there); the project can also be configured by hand:
#
#   cmake -S tests/disabled -B /tmp/disabled-check -DCSLG_SOURCE_DIR=$PWD \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build /tmp/disabled-check
#   tests/disabled/run_checks.sh /tmp/disabled-check/with_target \
#         /tmp/disabled-check/without_target /tmp/disabled-check/api_program

cmake_minimum_required(VERSION 3.14)
project(cslg_disabled_instrumentation_check CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CSLG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The call-stack-logger checkout to test")
set(DISABLE_INSTRUMENTATION ON CACHE BOOL "" FORCE)
add_subdirectory(${CSLG_SOURCE_DIR} call-stack-logger)

# The checks find the programs at the top of the build tree, whatever the
# generator: a multi-config one (Ninja Multi-Config, Visual Studio, Xcode)
# would put them under <config>/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" config)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}")
endforeach()

add_executable(with_target program.cpp)
target_link_libraries(with_target PRIVATE callstacklogger::instrumented)

add_executable(without_target program.cpp)

add_executable(api_program api_program.cpp)
target_link_libraries(api_program PRIVATE callstacklogger::callstacklogger)
