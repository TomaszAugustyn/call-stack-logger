#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# Unit tests — compiled WITHOUT instrumentation flags.
# Tests pure/deterministic functions from the include/ headers.

add_executable(unit_tests
    test_duration_format.cpp
    test_format.cpp
    test_pretty_time.cpp
    test_trace_file_path.cpp
)

target_include_directories(unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(unit_tests PRIVATE cxx_std_17)
target_link_libraries(unit_tests gtest_main)
cslg_apply_sanitizers(unit_tests)

if(COVERAGE)
    target_compile_options(unit_tests PRIVATE --coverage)
    target_link_options(unit_tests PRIVATE --coverage)
endif()

add_test(NAME unit_tests COMMAND unit_tests)
