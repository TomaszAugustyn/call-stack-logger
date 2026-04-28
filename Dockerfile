FROM ubuntu:24.04

# Install build dependencies:
# - g++, cmake: C++ build toolchain
# - binutils-dev: BFD library for symbol resolution
# - libc6-dbg: debug symbols for libc (required for BFD to resolve shared library symbols
#   without extremely slow fallback searches through many .build-id paths)
# - lcov: code coverage report generation
# - git: required by CMake FetchContent (used both internally for Google Test and by
#   user projects that integrate call-stack-logger via FetchContent_Declare).
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    clang \
    libclang-rt-18-dev \
    cmake \
    make \
    binutils-dev \
    libc6-dbg \
    lcov \
    git \
    locales \
    && rm -rf /var/lib/apt/lists/*

# Generate a comma-decimal locale (de_DE.UTF-8) so the DurationFormat locale-
# stress test actually runs under CI instead of skipping. The test guards
# format_duration_12chars() against any regression to a %f-based implementation
# that would emit ',' instead of '.' under LC_NUMERIC=de_DE.
RUN sed -i 's/# de_DE.UTF-8 UTF-8/de_DE.UTF-8 UTF-8/' /etc/locale.gen \
    && locale-gen

WORKDIR /workspace
