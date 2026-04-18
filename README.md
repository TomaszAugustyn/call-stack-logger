# Call Stack Logger #

![GCC](https://github.com/TomaszAugustyn/call-stack-logger/actions/workflows/ci.yml/badge.svg?branch=master&event=push)
<!-- Badges show GCC (build+test+coverage) and Clang (build+test) CI status -->

Call Stack Logger uses function instrumentation to facilitate logging of
every function call. Each nesting adds an ident, whereas returning from a
function removes it. As the result call stack tree is produced at the runtime
giving knowledge of the actual program's flow of execution.
## :seedling: Outcome ##

![Call Stack logger capture](misc/call-stack-logger-capture.gif)
## :book: Article ##

Here is the article on dev.to describing the details of the project, its aim and motivation
behind it: \
[Call Stack Logger - Function instrumentation as a way to trace program’s flow of execution](https://dev.to/taugustyn/call-stack-logger-function-instrumentation-as-a-way-to-trace-programs-flow-of-execution-419a)

## :scroll: Requirements ##

### GNU Binutils ###

It is required in order to get access to BFD (Binary File Descriptor
library) necessary to get information about object files and manipulate them.

```bash
sudo apt-get install binutils-dev
```

## :wrench: Building and running ##

```bash
git clone https://github.com/TomaszAugustyn/call-stack-logger.git
cd call-stack-logger

# Create build folder and go there
mkdir build && cd build

# Configure cmake with default logging (GCC is the default compiler)
cmake ..
# or to build with Clang instead of GCC
cmake -DCMAKE_CXX_COMPILER=clang++ ..
# or for extended logging you can play with these flags
cmake -DLOG_ADDR=ON -DLOG_NOT_DEMANGLED=ON ..
# or to compile your application with disabled instrumentation (no logging)
cmake -DDISABLE_INSTRUMENTATION=ON ..
# or to build with tests and code coverage
cmake -DBUILD_TESTS=ON -DCOVERAGE=ON ..

# Build
make

# Build and Run (as the result trace.out file will be generated)
make run
```

Each run appends to the trace output file with a timestamped separator header, so consecutive
runs are easy to distinguish. Output is line-buffered for crash safety without per-call overhead.
If the trace file cannot be opened, a warning is printed to `stderr`.

### Compiler-specific instrumentation ###

Both GCC and Clang use `-finstrument-functions` for user code and produce identical trace
output. The `callstacklogger` library itself is compiled without instrumentation flags (all its
functions have the `NO_INSTRUMENT` attribute). Standard library exclusion differs by compiler:

| | GCC | Clang |
|---|-----|-------|
| **Std library exclusion** | Compile-time (`-finstrument-functions-exclude-file-list`) | Runtime (mangled name filter in `resolve_function_name()`) |

GCC auto-discovers std library header paths and excludes them at compile time. Clang does
not support the exclude-file-list flag, so std library functions are filtered at runtime by
checking the Itanium C++ ABI mangled name for known `std::`, `__gnu_cxx::`, and
`__cxxabiv1::` prefixes.

When linking `callstacklogger` into custom programs, compile your application code with
`-finstrument-functions` (and GCC's exclude-file-list if using GCC) to enable tracing.

## :gear: Configuration ##

### CMake Options ###

| Option | Default | Description |
|--------|---------|-------------|
| `LOG_ADDR` | `OFF` | Include function addresses in trace output |
| `LOG_NOT_DEMANGLED` | `OFF` | Log functions even when demangling fails |
| `DISABLE_INSTRUMENTATION` | `OFF` | Compile without any instrumentation hooks |
| `BUILD_TESTS` | `OFF` | Build unit and integration tests (fetches Google Test) |
| `COVERAGE` | `OFF` | Enable code coverage via GCC `--coverage` flag |

### Environment Variables ###

| Variable | Default | Description |
|----------|---------|-------------|
| `CSLG_OUTPUT_FILE` | `trace.out` | Path to the trace output file for the main thread. Worker threads append `_tid_<gettid>` to this path (e.g., `/tmp/my_trace.out_tid_12345`). |

Example:
```bash
CSLG_OUTPUT_FILE=/tmp/my_trace.out ./build/runDemo
# Single-threaded program → /tmp/my_trace.out
# Multi-threaded program → /tmp/my_trace.out + /tmp/my_trace.out_tid_<N> per worker
```

## :shield: Thread Safety ##

Call Stack Logger has **full multi-threaded support**: each thread writes to its own
independent trace file. The main thread writes to `CSLG_OUTPUT_FILE` (or `trace.out`
fallback), and every worker thread writes to `<base>_tid_<gettid>` — e.g., if
`CSLG_OUTPUT_FILE=/tmp/prog.out`, a worker with kernel TID 12345 writes to
`/tmp/prog.out_tid_12345`. The numeric TID matches what `ps -L`, `top -H`, and
`/proc/<pid>/task/<tid>` show, making it easy to correlate a trace file with a specific
thread.

Each file starts with a header that includes the owning thread's ID:
```
================================================================
=== New trace run: 17-04-2026 15:18:12.255, thread ID: 12345 ===
================================================================
```

Per-thread state (`thread_local` call stack, re-entrancy guard) and per-thread `FILE*`
mean the hot write path has **zero cross-thread synchronization** — each thread writes
to its own file descriptor. BFD symbol resolution is still serialized by a mutex (BFD is
not thread-safe), but file I/O is fully parallel.

**Shutdown race (documented trade-off):** at program exit, the main thread closes all
trace files. If a worker is mid-`fprintf` when this happens, the worst case is one
torn line or one silent write to a just-closed fd (`EBADF`, no crash). Line-buffered
mode bounds corruption to at most one line per thread. This is acceptable for a
debugging tool and avoids a mutex on the hot write path.

`fork()` is not supported — the child process inherits the parent's `thread_local`
state including `FILE*` pointers pointing to inherited file descriptors.

## :test_tube: Testing ##

The project includes unit tests (for formatting and timestamp functions) and integration tests
(that compile an instrumented program, execute it, and verify the trace output). Tests use
the [Google Test](https://github.com/google/googletest) framework, fetched automatically via
CMake FetchContent on first build.

```bash
mkdir build && cd build
cmake -DBUILD_TESTS=ON ..
make
ctest --output-on-failure
```

To generate a code coverage report (requires `lcov` 2.0+):
```bash
cmake -B build -DBUILD_TESTS=ON -DCOVERAGE=ON
cmake --build build
cd build && ctest --output-on-failure && cd ..
lcov --capture --directory build --output-file coverage.info --ignore-errors mismatch
lcov --remove coverage.info '/usr/*' '*/tests/*' '*/_deps/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage-report
# Open coverage-report/index.html in a browser
```

The `callstacklogger` static library can also be linked into custom programs — link against
it and compile your application code with `-finstrument-functions` to enable tracing.

## :whale: Docker ##

Docker provides a reproducible build/test environment and is the recommended way to develop
on non-Linux platforms (macOS, Windows).

```bash
# Build the project (GCC, default)
docker compose run build

# Run all tests (unit + integration) with GCC
docker compose run test

# Run all tests with Clang
docker compose run test-clang

# Generate code coverage report (output in coverage-report/index.html)
docker compose run coverage
```

The Docker image is based on Ubuntu 24.04 with GCC, Clang, and all required dependencies,
including `libc6-dbg` (debug symbols for libc, required for fast BFD symbol resolution).

## :rocket: CI/CD ##

GitHub Actions runs on every push and pull request to `master`:
- **GCC job:** Builds, runs unit and integration tests, generates code coverage report
- **Clang job:** Builds and runs unit and integration tests

## :wrench: Building and running - legacy (Makefiles) ##

```bash
git clone https://github.com/TomaszAugustyn/call-stack-logger.git
cd call-stack-logger

mv Makefile_legacy Makefile
mv src/Makefile_legacy src/Makefile

# Build with default logging
make
# or for extended logging you can play with these flags
make log_with_addr=1 log_not_demangled=1
# or to compile your application with disabled instrumentation (no logging)
make disable_instrumentation=1

# Build and Run (as the result trace.out file will be generated)
make run
```

## :balance_scale: Copyright and License ##

Call Stack Logger is a single-copyright project: all the source code in this [Call Stack Logger
repository](https://github.com/TomaszAugustyn/call-stack-logger) is Copyright &copy; Tomasz Augustyn.

As copyright owner, I dual license Call Stack Logger under different license terms, and
offers the following licenses for Call Stack Logger:
- GNU AGPLv3, a popular open-source license with strong
[copyleft](https://en.wikipedia.org/wiki/Copyleft) conditions (the default license)
- Commercial or closed-source licenses

If you license Call Stack Logger under AGPLv3, there is no license fee or signed license
agreement: you just need to comply with the AGPLv3 terms and conditions. See
[LICENSE_COMMERCIAL](./LICENSE_COMMERCIAL) and [LICENSE](./LICENSE) for further information.

If you purchase a commercial or closed-source license for Call Stack Logger, you must comply
with the terms and conditions listed in the associated license agreement; the
AGPLv3 terms and conditions do not apply. To purchase commercial license please contact me via email at t.augustyn@poczta.fm in order to discuss requirements and formulate a commercial license that best suits your needs.

The Call Stack Logger software itself remains the same: the only difference between an open-source Call Stack Logger and a commercial Call Stack Logger are the license terms.
