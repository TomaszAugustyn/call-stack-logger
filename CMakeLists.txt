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

add_subdirectory(src)

set(EXECUTABLE runDemo)

add_custom_target(run
    COMMAND ${EXECUTABLE}
)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
