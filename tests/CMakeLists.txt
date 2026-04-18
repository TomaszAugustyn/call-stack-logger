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
add_subdirectory(integration)
