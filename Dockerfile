FROM ubuntu:24.04

# Install build dependencies:
# - g++, cmake: C++ build toolchain (binutils comes with g++: the tests use
#   objcopy and strip to build their fixtures; no binutils library is linked)
# - libc6-dbg: debug symbols for libc, so that frames inside libc get a file
#   and a line (the resolver finds them through /usr/lib/debug/.build-id)
# - lcov: code coverage report generation
# - ca-certificates: the HTTPS downloads CMake's FetchContent makes at
#   configure time (libbacktrace for the library, Google Test for the tests)
# - git: for user projects that integrate call-stack-logger via
#   FetchContent_Declare(GIT_REPOSITORY ...)
# - xz-utils: compresses the MiniDebugInfo of one test fixture (the test
#   skips without it)
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    clang \
    libclang-rt-18-dev \
    cmake \
    make \
    libc6-dbg \
    lcov \
    ca-certificates \
    git \
    xz-utils \
    locales \
    && rm -rf /var/lib/apt/lists/*

# Generate a comma-decimal locale (de_DE.UTF-8) so the DurationFormat locale-
# stress test actually runs under CI instead of skipping. The test guards
# format_duration_12chars() against any regression to a %f-based implementation
# that would emit ',' instead of '.' under LC_NUMERIC=de_DE.
RUN sed -i 's/# de_DE.UTF-8 UTF-8/de_DE.UTF-8 UTF-8/' /etc/locale.gen \
    && locale-gen

WORKDIR /workspace
