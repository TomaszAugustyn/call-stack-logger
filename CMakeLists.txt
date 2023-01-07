cmake_minimum_required(VERSION 3.10)

project(call-stack-logger)

add_subdirectory(src)

set(EXECUTABLE runDemo)

add_custom_target(run
    COMMAND ${EXECUTABLE}
)
