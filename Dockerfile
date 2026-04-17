FROM ubuntu:24.04

# Install build dependencies:
# - g++, cmake: C++ build toolchain
# - binutils-dev: BFD library for symbol resolution
# - libc6-dbg: debug symbols for libc (required for BFD to resolve shared library symbols
#   without extremely slow fallback searches through many .build-id paths)
# - lcov: code coverage report generation
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    clang \
    cmake \
    make \
    binutils-dev \
    libc6-dbg \
    lcov \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
