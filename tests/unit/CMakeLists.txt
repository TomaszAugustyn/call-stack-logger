#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# Unit tests — compiled WITHOUT instrumentation flags.
# Tests pure/deterministic functions from the include/ headers.
# The cslg_ prefix on the target and ctest name keeps it distinct from a
# parent project's own "cslg_unit_tests" target.

add_executable(cslg_unit_tests
    test_duration_format.cpp
    test_format.cpp
    test_pretty_time.cpp
    test_std_symbol_filter.cpp
    test_trace_file_path.cpp
)

target_include_directories(cslg_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(cslg_unit_tests PRIVATE cxx_std_17)
target_link_libraries(cslg_unit_tests gtest_main)
cslg_apply_sanitizers(cslg_unit_tests)

add_test(NAME cslg_unit_tests COMMAND cslg_unit_tests)
