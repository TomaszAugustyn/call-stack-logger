# Call Stack Logger - Project Guide

## Project Overview

Call Stack Logger is a C++ instrumentation framework that traces a program's flow of execution
at runtime by logging every function call. It leverages GCC/Clang's `-finstrument-functions`
compiler flag to automatically insert profiling hooks into every function entry and exit. The output is a
hierarchical call-stack tree written to `trace.out`, showing timestamps, function names, source
file locations, line numbers, and visual nesting depth.

**Author:** Tomasz Augustyn (t.augustyn@poczta.fm)
**Repository:** https://github.com/TomaszAugustyn/call-stack-logger
**Article:** https://dev.to/taugustyn/call-stack-logger-function-instrumentation-as-a-way-to-trace-programs-flow-of-execution-419a
**License:** Dual-licensed under GNU AGPLv3 (default) and commercial/closed-source (contact author)
**Language Standard:** C++17
**Target Platform:** Linux (relies on `/proc/self/cmdline`, `/proc/self/exe`, `dladdr`, BFD library)

## Architecture & How It Works

### Core Mechanism: GCC Function Instrumentation

The compiler flag `-finstrument-functions` causes GCC to inject calls to two special functions
at every function entry and exit:

- `__cyg_profile_func_enter(void *callee, void *caller)` - called on function entry
- `__cyg_profile_func_exit(void *callee, void *caller)` - called before function exit

Both receive raw `void*` pointers: `callee` is the address of the function being called,
`caller` is the return address (call site). The framework resolves these addresses into
human-readable function names, source filenames, and line numbers.

### Symbol Resolution Pipeline

Address resolution happens in multiple stages (implemented in `src/callStack.cpp`):

1. **`dladdr()`** (from `<dlfcn.h>`) - Retrieves basic symbol info from the dynamic linker,
   including the shared object base address (`dli_fbase`) and symbol name (`dli_sname`).

2. **BFD Library** (`<bfd.h>` from GNU binutils) - Opens the object file and reads its symbol
   table. Uses `bfd_find_nearest_line()` to map an address offset within a section to a source
   filename, function name, and line number. BFD objects are cached in a static
   `std::map<void*, storedBfd>` so each object file is loaded only once.

3. **`abi::__cxa_demangle()`** (from `<cxxabi.h>`) - Converts GCC-mangled C++ symbol names
   (e.g., `_ZN1A3fooEv`) into human-readable form (e.g., `A::foo()`).

### Stack Unwinding for Accurate Caller Location

The `caller` address from `__cyg_profile_func_enter` points to the instrumentation call site,
not the original source location. To get the real caller, the framework uses `_Unwind_Backtrace`
and `_Unwind_GetIPInfo` from `<unwind.h>` to walk the stack to frame 6.

The constant frame depth of 6 is derived from this fixed call chain:
```
Frame 6: instrumentation::FrameUnwinder::unwind_nth_frame
Frame 5: bfdResolver::resolve / instrumentation::unwind_nth_frame
Frame 4: instrumentation::bfdResolver::resolve
Frame 3: instrumentation::resolve
Frame 2: __cyg_profile_func_enter
Frame 1: The actual user function being called
```

**IMPORTANT:** If the resolution pipeline code is modified (adding/removing function calls in the
chain), the frame number (currently 6) in `callStack.cpp:209` MUST be recalculated.

### Excluding Standard Library from Instrumentation

Standard library functions (e.g., `std::sort`, `std::vector::push_back`) must be excluded
from instrumentation — otherwise they flood the trace with internal implementation details.
The project uses a **two-tier exclusion approach** depending on the compiler:

**GCC (compile-time exclusion):** The build system auto-discovers C++ standard library header
paths using `${CMAKE_CXX_COMPILER} -xc++ -E -v -`, then passes them via
`-finstrument-functions-exclude-file-list=<paths>`. The project's own instrumentation headers
are also excluded: `callStack.h`, `unwinder.h`, `types.h`, `format.h`, `prettyTime.h`.
With this approach, GCC never inserts instrumentation hooks into std library functions.

**Clang (runtime exclusion):** Clang does not support `-finstrument-functions-exclude-file-list`.
Instead, std library template instantiations compiled into the user's translation unit receive
hooks and trigger `__cyg_profile_func_enter` at runtime. The function `is_std_library_symbol()`
in `callStack.cpp` (compiled under `#ifdef __clang__`) checks the Itanium C++ ABI mangled name
for known std library prefixes before expensive BFD resolution:
- `__cxa_*` — C++ ABI runtime functions
- `_Z[N[cv]]St*` — `std::` functions and members
- `_Z[N[cv]]S[absiod]*` — std substitutions (allocator, basic_string, string, etc.)
- `_Z[N[cv]]9__gnu_cxx*` — GNU C++ extensions (`__normal_iterator`, etc.)
- `_Z[N[cv]]10__cxxabiv1*` — C++ ABI internals
- `_Z[N[cv]]11__gnu_debug*` — GNU debug-mode containers
- `_ZZ` prefix — local entities inside std library functions (e.g., `_Guard` classes)

Where `[cv]` = optional cv-qualifiers (K=const, V=volatile, r=restrict).

This filter runs inside `resolve_function_name()` right after `dladdr()` and before BFD
loading, so filtered functions avoid the expensive BFD symbol resolution entirely.

### The NO_INSTRUMENT Macro

```cpp
#define NO_INSTRUMENT __attribute__((no_instrument_function))
```

Applied to all functions in the instrumentation/resolution pipeline to prevent recursive
instrumentation (which would cause infinite recursion and stack overflow).

### Trace File Lifecycle

`trace.cpp` uses GCC `__attribute__((constructor))` and `__attribute__((destructor))` to
open the trace output file before `main()` runs and close it after the program exits. The
output path is configurable via the `CSLG_OUTPUT_FILE` environment variable (defaults to
`"trace.out"` in the current working directory). The file is opened with `O_NOFOLLOW` to
prevent symlink-based attacks and uses `0644` permissions. All trace output is appended to
this file. Each run writes a timestamped separator header so consecutive runs are
distinguishable. Output is line-buffered (`_IOLBF`) for crash safety without per-call
`fflush` overhead. If the file cannot be opened, a warning is printed to `stderr`.

### Indentation / Nesting Depth

A `thread_local` `current_stack_depth` counter in `trace.cpp`:
- Increments on each successfully resolved function entry
- Decrements on function exit (only if the entry was resolved)
- A fixed-size `frame_resolved_stack[2048]` array (also `thread_local`) tracks per-frame
  whether the entry was resolved: every `__cyg_profile_func_enter` pushes (resolved or not),
  every `__cyg_profile_func_exit` pops, and depth is only adjusted for resolved frames
- An overflow counter handles the edge case when call depth exceeds `MAX_TRACE_DEPTH` (2048),
  preventing stack desynchronization. Indentation remains correct at any depth.

The `utils::format()` function in `format.h` uses this depth to produce tree-style indentation
with `|  ` and `|_ ` prefixes.

### Output Format

Each line in `trace.out` follows this pattern:
```
[DD-MM-YYYY HH:MM:SS.mmm] |_ functionName  (called from: filename.cpp:42)
```

Nested calls are indented with `|  ` prefixes, creating a visual call tree:
```
[14-03-2021 01:12:00.231] |_ main  (called from: /build/glibc-.../csu/../libc-start.c:310)
[14-03-2021 01:12:00.231] |  |_ A::foo()  (called from: .../src/main.cpp:28)
[14-03-2021 01:12:00.231] |  |_ B::foo()  (called from: .../src/main.cpp:33)
[14-03-2021 01:12:00.231] |  |  |_ A::foo()  (called from: .../src/main.cpp:18)
[14-03-2021 01:12:00.231] |  |_ fibonacci(int)  (called from: .../src/main.cpp:23)
[14-03-2021 01:12:00.231] |  |  |_ fibonacci(int)  (called from: .../src/main.cpp:23)
[14-03-2021 01:12:00.231] |  |  |  |_ fibonacci(int)  (called from: .../src/main.cpp:23)
```

With optional address logging when `LOG_ADDR` is defined:
```
[DD-MM-YYYY HH:MM:SS.mmm] addr: [0x00007fff12345678] |_ functionName  (called from: filename.cpp:42)
```

## File Structure

```
call-stack-logger/
|-- CMakeLists.txt              # Root CMake config (project definition, run target)
|-- Dockerfile                  # Ubuntu 24.04 build environment
|-- docker-compose.yml          # Build, test, coverage services
|-- Makefile_legacy             # Legacy root Makefile (delegates to src/)
|-- README.md                   # Project overview, build instructions
|-- CONTRIBUTING.md             # Contribution guidelines
|-- CONTRIBUTOR_AGREEMENT.md    # CLA for contributors
|-- LICENSE                     # GNU AGPLv3 full text
|-- LICENSE_COMMERCIAL          # Commercial license placeholder
|-- .clang-format               # Code formatting rules (WebKit-based, 110 col limit)
|-- .gitignore                  # Ignores build artifacts, IDE files, *.out
|-- include/
|   |-- callStack.h             # bfdResolver struct, get_call_stack(), resolve() API
|   |-- format.h                # utils::format() - formats ResolvedFrame into string
|   |-- prettyTime.h            # utils::pretty_time() - timestamp with milliseconds
|   |-- types.h                 # ResolvedFrame struct definition
|   |-- unwinder.h              # FrameUnwinder template, Callback, unwind_nth_frame()
|-- src/
|   |-- CMakeLists.txt          # Build config (flags, std lib exclusion, library + executable)
|   |-- Makefile_legacy         # Legacy Makefile with same logic
|   |-- callStack.cpp           # Core implementation: BFD loading, symbol resolution
|   |-- trace.cpp               # __cyg_profile_func_enter/exit, trace file I/O
|   |-- main.cpp                # Demo program exercising various C++ features
|-- tests/                      # Unit and integration tests (BUILD_TESTS=ON)
|   |-- CMakeLists.txt          # FetchContent for Google Test, add subdirs
|   |-- unit/
|   |   |-- CMakeLists.txt      # Unit test executable (no instrumentation)
|   |   |-- test_format.cpp     # Tests for utils::format()
|   |   |-- test_pretty_time.cpp # Tests for utils::pretty_time() and to_ms()
|   |-- integration/
|       |-- CMakeLists.txt      # Traced program + integration test runner
|       |-- traced_program.cpp  # Small instrumented program for testing
|       |-- test_integration.cpp # Tests that run traced_program, parse trace output
|-- lib/                        # Empty dir (placeholder for external libs)
|   |-- .gitignore              # Keeps dir in git but ignores contents
|-- misc/
|   |-- call-stack-logger-capture.gif  # Demo capture for README
|-- .github/
    |-- workflows/
        |-- ci.yml              # GitHub Actions: build, test, code coverage
```

## Key Source Files In Detail

### `include/types.h`
Defines `instrumentation::ResolvedFrame` struct with fields:
- `timestamp` (string), `callee_address` (optional void*), `callee_function_name` (string),
  `caller_filename` (string), `caller_line_number` (optional unsigned int)

### `include/callStack.h`
Declares the `bfdResolver` struct with:
- `storedBfd` inner struct wrapping a `bfd*` with unique_ptr and symbol table
- Static methods: `ensure_bfd_loaded()`, `resolve()`, `resolve_function_name()`,
  `resolve_filename_and_line()`, `check_bfd_initialized()`, `get_argv0()`,
  `ensure_actual_executable()`
- Static state: `s_bfds` (map cache), `s_bfd_initialized`, `s_argv0`
- Free functions: `get_call_stack()`, `resolve()`

### `include/unwinder.h`
Template class `FrameUnwinder<F>` that uses `_Unwind_Backtrace` to walk to the N-th stack
frame and invoke a callback. The `Callback` struct captures the caller address.

### `include/format.h`
`utils::format()` takes a `ResolvedFrame` and `current_stack_depth`, produces the formatted
log line with optional address, tree indentation, function name, and caller location.

### `include/prettyTime.h`
`utils::pretty_time()` returns current time as `"DD-MM-YYYY HH:MM:SS.mmm"` string.
**Note:** Uses `std::localtime()` which is not thread-safe.

### `src/callStack.cpp`
The core implementation. Key functions:
- `demangle_cxa()` - Wraps `abi::__cxa_demangle` with RAII
- `is_std_library_symbol()` - (Clang only, `#ifdef __clang__`) Checks Itanium C++ ABI mangled
  names for std library prefixes; filters std:: functions before expensive BFD resolution
- `bfdResolver::ensure_bfd_loaded()` - Opens BFD, reads symbol table, caches by base address
- `bfdResolver::resolve_function_name()` - Resolves callee address to demangled function name
- `bfdResolver::resolve_filename_and_line()` - Resolves caller address to source file and line
- `bfdResolver::resolve()` - Orchestrates full resolution including stack unwinding
- `bfdResolver::get_argv0()` - Reads `/proc/self/cmdline` for executable path
- `bfdResolver::ensure_actual_executable()` - Handles PATH-found executables via `/proc/self/exe`
- `get_call_stack()` - Uses `backtrace()` to build full call stack (max 1000 frames)

### `src/trace.cpp`
The instrumentation entry points:
- `trace_begin()` - Constructor: opens trace file, writes run separator, warns on failure
- `trace_end()` - Destructor: closes trace file
- `__cyg_profile_func_enter()` - Resolves, logs function entry, pushes to resolution stack
- `__cyg_profile_func_exit()` - Pops resolution stack, decrements depth if frame was resolved
- Guarded by `#ifndef DISABLE_INSTRUMENTATION`

### `src/main.cpp`
Demo program testing instrumentation with:
- Static member method (`A::foo()`)
- Lambda expressions
- Constructor (`B::B()`)
- Non-static methods with STL usage (`B::foo()` with `std::sort`)
- Constexpr function (`fibonacci(6)`)
- Variadic function templates (`print()`)
- Inline function (`cube()`)

## Build System

### CMake (Primary)

```bash
mkdir build && cd build
cmake ..                                    # Default logging
cmake -DLOG_ADDR=ON ..                      # Include addresses in output
cmake -DLOG_NOT_DEMANGLED=ON ..             # Log even undemangled functions
cmake -DLOG_ADDR=ON -DLOG_NOT_DEMANGLED=ON ..  # Both flags
cmake -DDISABLE_INSTRUMENTATION=ON ..       # No instrumentation at all
make                                        # Build
make run                                    # Build and run (generates trace.out)
```

### Legacy Makefiles

Rename `Makefile_legacy` -> `Makefile` and `src/Makefile_legacy` -> `src/Makefile`, then:
```bash
make                                    # Default
make log_with_addr=1                    # With addresses
make log_not_demangled=1                # With undemangled names
make disable_instrumentation=1          # No instrumentation
make run                                # Run
```

### Compilation Flags

| Flag | Purpose |
|------|---------|
| `-finstrument-functions` | Enable function entry/exit hooks (user code, both compilers) |
| `-finstrument-functions-exclude-file-list=...` | Exclude std lib and project headers (GCC only) |
| `-rdynamic` | Export all symbols to dynamic symbol table (needed by `dladdr`) |
| `-g` | Debug symbols (needed by BFD for line numbers) |
| `-Wall` | All warnings |
| `-std=c++17` | C++17 standard (for `std::optional`, structured bindings, etc.) |

### Preprocessor Defines

| Define | Effect |
|--------|--------|
| `LOG_ADDR` | Include function addresses in trace output |
| `LOG_NOT_DEMANGLED` | Log functions even when demangling fails |
| `DISABLE_INSTRUMENTATION` | Compile without any instrumentation hooks |

### Link Dependencies

| Library | Purpose |
|---------|---------|
| `-ldl` | Dynamic linking (`dladdr`) |
| `-lbfd` | Binary File Descriptor (symbol/line resolution) |

### System Requirement

- **GNU Binutils dev package:** `sudo apt-get install binutils-dev` (provides `libbfd`)

### Library Target

The build produces a `callstacklogger` static library from `callStack.cpp` and `trace.cpp`.
The `runDemo` executable and test programs link against this library. Custom programs can
also link against it — compile application code with `-finstrument-functions` and the
appropriate exclude-file-list, then link against `callstacklogger`.

## Testing

Tests are built when `BUILD_TESTS=ON` is passed to CMake. Google Test is fetched via
FetchContent (downloaded once, cached for offline use).

### Unit Tests (`tests/unit/`)

Test pure/deterministic functions from the include headers:
- `test_format.cpp` — tree indentation, address formatting, line numbers, buffer handling
- `test_pretty_time.cpp` — timestamp format, length, milliseconds, `to_ms()` conversion

### Integration Tests (`tests/integration/`)

- `traced_program.cpp` — small instrumented program with varied call patterns (free functions,
  static methods, templates, constructors, inline functions, STL usage via `func_with_stl()`)
- `test_integration.cpp` — executes `traced_test_program`, parses trace output, verifies:
  function names resolved, nesting depth correct, caller info present, timestamp format,
  run separator, CSLG_OUTPUT_FILE redirection, std library functions excluded,
  exact trace line count (catches std library pollution regressions)
- Built as two targets: `traced_test_program` (compiled WITH `INSTRUMENT_FLAGS`) and
  `noninstrumented_test_program` (compiled WITHOUT — simulates `DISABLE_INSTRUMENTATION`).
  The `DisableInstrumentationTest.NoTraceOutputWithoutInstrumentation` test runs the
  non-instrumented version and verifies zero trace entries are produced.

### Running Tests

```bash
cmake -DBUILD_TESTS=ON ..
make
ctest --output-on-failure
```

### Code Coverage

Requires `lcov` 2.0+ (Ubuntu 24.04, Fedora 40+). Older lcov versions use different flags.

```bash
cmake -B build -DBUILD_TESTS=ON -DCOVERAGE=ON
cmake --build build
cd build && ctest --output-on-failure && cd ..
lcov --capture --directory build --output-file coverage.info --ignore-errors mismatch
lcov --remove coverage.info '/usr/*' '*/tests/*' '*/_deps/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage-report
```

Notes:
- `--ignore-errors mismatch` bypasses lcov 2.0 strict checks that fail on gtest code.
- `--no-external` is NOT used: in lcov 2.0 it uses `--directory` as the base for
  "external" checks, which would wrongly exclude our `src/` files (outside `build/`).
  Instead, explicitly filter system headers via `--remove '/usr/*'`.

## CI/CD

GitHub Actions (`.github/workflows/ci.yml`) runs on push/PR to `master`:
- Builds with `BUILD_TESTS=ON` and `COVERAGE=ON` on Ubuntu
- Runs unit and integration tests via `ctest`
- Generates lcov HTML coverage report (uploaded as artifact)

## Code Style

- **Formatter:** clang-format with WebKit-based style
- **Column limit:** 110
- **Indent:** 4 spaces (no tabs)
- **Braces:** Attach style (K&R / same-line)
- **Pointer alignment:** Left (`int* ptr`)
- **Namespace indentation:** None
- **Short functions:** Inline only allowed on single line
- **Include sorting:** Enabled
- **Template declarations:** Always break after

## Namespaces

- `instrumentation::` - All symbol resolution and call stack logic
- `utils::` - Formatting and time utilities
- Anonymous namespace in `callStack.cpp` for `demangle_cxa()` and `is_std_library_symbol()`
  (Clang only)

## Design Decisions & Caveats

1. **Linux-only:** Relies on `/proc/self/cmdline`, `/proc/self/exe`, `dladdr`, BFD,
   `_Unwind_*`, `localtime_r`, `O_NOFOLLOW`. Supports both GCC and Clang compilers.
   **Std library exclusion uses a two-tier approach:**
   - GCC: compile-time via `-finstrument-functions-exclude-file-list` (auto-discovered paths)
   - Clang: runtime via `is_std_library_symbol()` mangled name filter in `resolve_function_name()`
   Both compilers use plain `-finstrument-functions` for user code. The callstacklogger library
   is compiled WITHOUT instrumentation flags (split compilation) because all its functions
   have `NO_INSTRUMENT`. This avoids linker errors with constexpr libstdc++ functions (e.g.,
   `basic_string::_M_init_local_buf`) that have no out-of-line symbol in libstdc++.so — the
   library uses `std::string` extensively but doesn't need instrumentation. A thread_local
   re-entrancy guard in
   `__cyg_profile_func_enter` prevents recursive instrumentation from within the resolve
   pipeline (critical for Clang). Shutdown uses `atexit()` to disable instrumentation before
   static destructors run.
2. **Multithreaded-ready:** Per-thread call stack state uses `thread_local`; shared resources
   (`fp_trace`, BFD library) are protected by `std::mutex`; `localtime_r` replaces
   `std::localtime`. All threads currently write to a single shared trace file with serialized
   access. **Per-thread trace files are a future enhancement** for full multi-threaded support.
   `s_bfd_mutex` is held for the entire `resolve()` call (covering initialization, loading,
   section iteration, and `bfd_find_nearest_line`) since BFD is not thread-safe.
   `fp_trace` is double-checked (outside lock as fast path, inside lock as authoritative check)
   to prevent TOCTOU races with `trace_end()`.
3. **Frame 6 constant:** The unwinder hard-codes frame depth 6 - must be recalculated if the
   call chain between `__cyg_profile_func_enter` and `unwind_nth_frame` changes.
   Verified correct after all Phase 1, Phase 2, and Phase 3 changes (no commit touched the
   call chain).
4. **Append mode:** Trace output is opened with `O_APPEND | O_NOFOLLOW` - multiple runs
   accumulate, separated by timestamped headers; output is line-buffered (`_IOLBF`)
   with `0600` permissions (owner read/write only)
5. **Configurable output:** Set `CSLG_OUTPUT_FILE` environment variable to redirect trace
   output to a custom path (defaults to `"trace.out"`)
6. **Performance overhead:** Every function call triggers symbol resolution via BFD; this tool
   is for debugging/tracing, not production use. `format()` uses `snprintf` with a stack
   buffer to avoid per-call heap allocation.
7. **Header-only utilities:** `format.h`, `prettyTime.h`, `unwinder.h` contain inline
   implementations in headers (definitions in headers, not just declarations)

## Use Cases (from the article)

The tool is designed for scenarios where traditional debugging is impractical:
- Problem reproducible only on customer/remote infrastructure
- Lacking necessary test setup to reproduce locally
- Bug lies in third-party library code without source access
- Need to understand flow of execution in large, unfamiliar codebases
- Step-through debugging too slow or not available

## Git History Summary

The project evolved through ~30 commits from initial commit to current state:
1. Initial commit with basic class/function name printing
2. Added standard library exclusion from instrumentation
3. Added filename and line number resolution via BFD
4. Added unwinder for correct caller location resolution
5. Added NO_INSTRUMENT macro, formatting, timestamps
6. Added optional LOG_ADDR and LOG_NOT_DEMANGLED flags
7. Migrated to CMake build system (Makefiles became legacy)
8. Added documentation, article link, licensing, contribution guidelines
