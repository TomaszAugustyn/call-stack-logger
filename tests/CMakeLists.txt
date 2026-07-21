#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm

# Fetch Google Test framework.
# Downloaded once during cmake configuration and cached in build/_deps/.
# Subsequent builds work fully offline.
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    # Set extracted file timestamps to extraction time (CMP0135 NEW behavior).
    # Without this, CMake warns on versions >= 3.24 and uses archive timestamps,
    # which can cause incremental builds to miss updates when the URL changes.
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Prevent gtest from overriding compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(unit)

# Integration tests execute instrumented programs and assert on their trace
# output; with DISABLE_INSTRUMENTATION=ON the hooks are compiled out, so the
# whole suite would fail by design. Unit tests (pure header functions,
# unaffected by the flag) still build and run.
if(DISABLE_INSTRUMENTATION)
    message(STATUS "DISABLE_INSTRUMENTATION=ON — skipping integration tests "
            "(they assert on trace output the disabled hooks can never produce)")
else()
    add_subdirectory(integration)
endif()
