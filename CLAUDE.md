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

Results are **memoized per address**: `resolve_no_unwind()` consults two hash maps
(callee address → demangled name, call-site address → file:line, both in
`callStack.h`) before running the stages above, so the full dladdr + BFD + demangle
pipeline executes only on first sight of each address — every repeat call is a hash
lookup under the same mutex. A cached `nullopt` name records "filtered / not
loggable", which also turns Clang's runtime std-library filter into a hash hit on
repeat calls. `bfdResolver::resolve()` additionally consults that cached-nullopt
state (via `is_cached_filtered()`) BEFORE running the stack unwinder, so
permanently-filtered callees — internal-linkage (`static`) functions on both
compilers, std-library instantiations on Clang — skip the per-call
`_Unwind_Backtrace` walk entirely; the helper returns before the unwind runs, so
it cannot shift the frame-6 constant. Cache growth is bounded by the program text (distinct instrumented
functions + call sites), not by runtime input; `dlclose`+`dlopen` address reuse
serves stale entries — the same accepted trade-off as the `s_bfds` object cache.

### Stack Unwinding for Accurate Caller Location

The `caller` address from `__cyg_profile_func_enter` points to the instrumentation call site,
not the original source location. To get the real caller, the framework uses `_Unwind_Backtrace`
and `_Unwind_GetIPInfo` from `<unwind.h>` to walk the stack to frame 6.

The constant frame depth of 6 is derived from this fixed call chain (innermost first,
matching the authoritative comment in `bfdResolver::resolve()`):
```
Frame 1: FrameUnwinder::unwind_nth_frame
Frame 2: instrumentation::unwind_nth_frame
Frame 3: bfdResolver::resolve
Frame 4: instrumentation::resolve
Frame 5: __cyg_profile_func_enter
Frame 6: the instrumented function itself (the callee)
Frame 7: the caller of the instrumented function — captured (one beyond the 6 increments)
```

**IMPORTANT:** If the resolution pipeline code is modified (adding/removing function calls in the
chain), the frame number (currently 6) passed to `unwind_nth_frame()` inside
`bfdResolver::resolve()` in `src/callStack.cpp` MUST be recalculated.

### Excluding Standard Library from Instrumentation

Standard library functions (e.g., `std::sort`, `std::vector::push_back`) must be excluded
from instrumentation — otherwise they flood the trace with internal implementation details.
The project uses a **two-tier exclusion approach** depending on the compiler:

**GCC (compile-time exclusion):** The build system auto-discovers C++ standard library header
paths using `${CMAKE_CXX_COMPILER} -xc++ -E -v -`, then passes them via
`-finstrument-functions-exclude-file-list=<paths>`. The project's own instrumentation headers
are also excluded: `callStack.h`, `unwinder.h`, `types.h`, `format.h`, `prettyTime.h`,
`traceFilePath.h`, `durationFormat.h`.
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

### Trace File Lifecycle — Per-Thread Files

`trace.cpp` gives every thread its own independent trace file:
- **Main thread** writes to `CSLG_OUTPUT_FILE` (fallback `trace.out`).
- **Worker threads** write to `<base>_tid_<gettid>` where `<gettid>` is the Linux
  kernel thread ID from `syscall(SYS_gettid)`.

The main thread's file is opened eagerly in `trace_begin()` (via
`__attribute__((constructor))`, before `main()` runs). Worker threads lazily open their
file on the first `__cyg_profile_func_enter` call for that thread.

All files are opened with `O_NOFOLLOW` (prevents symlink attacks), `O_CLOEXEC` (an
exec'd child of the traced program must not inherit writable trace descriptors — this
covers the LOG_ELAPSED patch_fd too), and `0600` permissions.
Output is line-buffered (`_IOLBF`) for crash safety without per-call `fflush` overhead.

Each file's header identifies the owning thread:
```
================================================================
=== New trace run: 17-04-2026 15:18:12.255, thread ID: 12345 ===
================================================================
```

The framing `=` lines are sized to match the middle line's length.

**State organization** (`trace.cpp`):
- `PerThreadState` struct: bundles all thread-local state — `in_instrumentation` guard
  (declared first so it's destroyed last), `current_stack_depth`, `frame_resolved_stack`,
  `frame_overflow_count`, `cached_tid`, and a `PerThreadTraceFile` RAII wrapper around
  the thread's FILE*. One `thread_local` instance: `t_state`.
- `TraceGlobals` struct: bundles process-wide state — `main_tid`, `base_path`,
  `open_files_mutex`, `open_files` registry (vector of `PerThreadTraceFile*`),
  and three atomic flags (`trace_ready`, `shutdown_started`, `shutdown_complete`).
  Accessed via a lazily-initialized singleton function `g_trace()` because the struct
  contains `std::string` and `std::vector` which need dynamic initialization — a plain
  file-scope static would not be fully constructed when `trace_begin()` (a constructor
  function) runs, causing a segfault on assignment. The instance is heap-allocated and
  deliberately leaked: a worker thread exiting during the tail of static destruction
  still locks `open_files_mutex` in `~PerThreadTraceFile`, which would be UB on a
  destroyed function-local static. The kernel reclaims the memory at process exit and
  LSan counts a reachable global as live, so nothing is reported as leaked.

**Write path: zero cross-thread synchronization.** Each thread calls `get_thread_fp()`
to obtain its own `FILE*` (lazy-opened on first use) and writes directly without any
mutex. The only mutex (`open_files_mutex`) is taken only during open (once per thread)
and during shutdown.

The `fp` field inside `PerThreadTraceFile` is `std::atomic<FILE*>` with
`memory_order_relaxed` reads/writes. The owning thread reads it lock-free on the
hot path; its own `~PerThreadTraceFile` (thread exit) nulls it under
`open_files_mutex`. Declaring it atomic avoids any C++ memory-model hair-splitting
and on x86_64 / ARM64 compiles to a single `mov` / `LDR` — no hot-path overhead.
(Earlier revisions had `trace_shutdown` cross-thread null the pointer; see below
for why that was changed.)

**Shutdown coordination (no cross-thread close)**:
- `atexit(trace_shutdown)` registered in `trace_begin()` — runs BEFORE static
  destructors (critical for Clang where those destructors may be instrumented).
- `trace_shutdown()` acquires `open_files_mutex`, iterates the registry, calls
  `fflush()` on each thread's `FILE*` (pushes any buffered partial line to the
  kernel — thread-safe, per-FILE implicit lock), then clears the registry and
  sets `shutdown_complete`. Idempotent via a CAS on `shutdown_started`.
- `trace_shutdown` deliberately does NOT `fclose()` other threads' fps or
  `close()` their patch_fds. `fclose` frees the stdio FILE struct — if a worker
  thread were mid-`fprintf` when shutdown ran, its next stdio call would be a
  use-after-free. `close(patch_fd)` would release the fd number for reuse —
  a stale `pwrite` from a racing worker could then land in an unrelated file.
  Both outcomes are worse than the documented "EBADF at worst" guarantee.
- Per-thread `~PerThreadTraceFile` closes both fds on **thread exit**. That
  runs only on the owning thread, so no cross-thread race exists there. Threads
  still alive at process exit have their fds closed by the kernel.
- The cost is one fd pair per trace-producing thread held open until process
  exit — trivial for a debug tool.

**Shutdown race (accepted trade-off):** a worker may be mid-`fprintf` when main
runs `trace_shutdown`. Worst case is a torn final line: the fprintf completes
normally against a still-valid FILE struct and still-open fd; the line just
lands after the shutdown's fflush. `shutdown_complete` is observed by subsequent
calls to `get_thread_fp()` within one cache coherence delay, which returns
nullptr and stops new writes. A per-file mutex was considered but rejected as
overengineering for a debugging tool.

`fork()` is not supported.

### Per-Function Timing (LOG_ELAPSED)

Opt-in via `-DLOG_ELAPSED=ON`. When enabled, every traced function's exit
patches the corresponding line on disk in place with the elapsed duration —
no second line, no reorder, no growth in line count.

**Two-fd-per-thread design.** Each thread opens its trace file twice:
the existing `FILE* fp` with `O_APPEND` (used for sequential writes —
header lines, enter lines) and a new raw `int patch_fd` **without**
`O_APPEND`, used only for `pwrite()`. `patch_fd` is obtained by
reopening `/proc/self/fd/<fd>` of the already-open descriptor — the
magic symlink resolves to the open inode itself, guaranteeing both
descriptors reference the SAME file even if the path is swapped between
the opens (TOCTOU), while giving a separate open file description so
clearing `O_APPEND` doesn't affect `fp` (`dup()` couldn't do that).
Without the second fd, `pwrite` on Linux is silently redirected to EOF
when the fd has `O_APPEND` — see `pwrite(2)`. The two fds never overlap
in byte range: `fp` only writes **new** bytes beyond EOF, `patch_fd`
only rewrites **existing** placeholder bytes. If the `/proc/self/fd`
reopen fails (no `/proc` in a minimal chroot/container, fd exhaustion),
the thread degrades instead of losing tracing: `fp` stays open,
`patch_fd` stays `-1`, `cursor_valid` drops, and every line keeps its
`[  pending ]` placeholder — the same degraded mode as a mid-run write
failure, announced once on stderr.

**Fixed-width invariant.** Every duration field is exactly 12 bytes
(`DURATION_FIELD_WIDTH` in `include/durationFormat.h`). The placeholder
`[  pending ]`, the saturation sentinel `[  >999.9s ]`, and every output
of `format_duration_12chars()` match that width — `static_assert`s and
the `DurationFormatTest` suite enforce this. If it ever broke, pwrite
would shift bytes and corrupt the file; the invariant is the safety net.

**Locale-independent format.** `format_duration_12chars` uses integer math
and the `PRIu64` integer specifier — deliberately avoids `%f`, which
honors `LC_NUMERIC` (decimal separator drifts to `,` in `de_DE` / `pl_PL` /
`fr_FR` etc.). The decimal point in the format string is a literal `.`
character, so the output is byte-identical across locales.
`DurationFormatTest.DecimalSeparatorIsLocaleIndependent` switches
`LC_NUMERIC=de_DE.UTF-8` at runtime and asserts the field still contains
`.` — the Dockerfile generates the locale so CI exercises this path.

**Cursor tracking, no ftello/fflush.** A `thread_local off_t cursor`
seeded from `lseek(patch_fd, 0, SEEK_END)` after the run-separator
header is written. Every line write advances `cursor` by the exact byte
count. Placeholder offset for a frame is then
`cursor_before_write + utils::PRETTY_TIME_LENGTH + 3` — the field is
spliced immediately after `"[<timestamp>] "`, before the optional
`addr:` column and the tree, which keeps the offset invariant under
any combination of `LOG_ADDR` / `LOG_NOT_DEMANGLED`.

**Cursor integrity on write failure.** Each line goes out as one
`fwrite` of the newline-terminated string, and the returned byte count
is checked (the `'\n'` is the line-buffered flush point, so ENOSPC/EIO
surface as a short count). On the first failed write — or if the initial lseek
cannot determine EOF (non-seekable target) — the per-thread
`cursor_valid` flag drops permanently: an unknown number of bytes
reached the file, so every later cursor-derived offset would be a
guess. From then on new frames record a `-1` offset sentinel and the
exit handler skips their pwrite — their `[  pending ]` stays, the
documented degraded mode. Frames recorded while the cursor was still
valid keep correct offsets and are still patched. This prevents
duration bytes from being spliced into the middle of other lines
after a disk-full event.

**Crash-diagnostic feature.** A line on disk holds either a real
`[   1.234ms]` (function returned cleanly) or `[  pending ]` (still
executing at the crash). On crash, the chain of `[  pending ]` lines
near the tail identifies exactly which frames were active at the crash
site — useful debugging signal that's hard to get otherwise. See README's
"Per-function timing" section for an example.

**Per-frame state.** Two thread_local arrays parallel to
`frame_resolved_stack[]`:
`std::chrono::steady_clock::time_point frame_enter_time[MAX_TRACE_DEPTH]`
and `off_t frame_placeholder_offset[MAX_TRACE_DEPTH]`. Indexed by
`frame_resolved_top` so enter/exit operate on the matching slot. The
exit handler computes `now() - enter_time[idx]`, formats into a 12-byte
buffer, and `pwrite(patch_fd, buf, 12, frame_placeholder_offset[idx])`.

**What the span measures.** `frame_enter_time` is filled as the enter
hook's LAST step for the frame (after resolve, format, and the line
write) and the exit hook reads the clock FIRST (before its own format +
pwrite). A frame's reported duration therefore excludes the tracer's own
per-call work for that frame — without this, every leaf function's
duration would be dominated by hook overhead (microseconds even with
warm memoization caches, tens of microseconds cold). Hook overhead of
calls nested inside the function still lands in the parent's span;
parent ≥ child always holds because a child's whole span (hooks
included) sits inside the parent's body.

**Overflow frames** (call depth > MAX_TRACE_DEPTH = 2048) still get an
enter line written with the `[  pending ]` placeholder so tree indentation
remains visually correct, but they do not get a slot in the per-frame
arrays — writing one would overwrite the topmost pushed frame's slot and
corrupt its patch. The `frame_resolved_top` bounds check in the enter
handler enforces this. Their placeholders therefore remain `[  pending ]` even
on clean exit. This is an accepted artifact of MAX_TRACE_DEPTH; such
deep stacks already lose resolve-state tracking.

**Drift guards.** Both the `PrettyTimeTest.LengthMatchesConstant` unit
test and an always-on runtime check in `trace_begin()` verify that
`pretty_time().size() == utils::PRETTY_TIME_LENGTH` — if anyone changes
either of the time-format strings without updating the constant, the
test fails (and the program aborts at startup with a FATAL message)
rather than silently corrupting trace files. The runtime check is a
manual if+abort rather than `assert()` so it also holds in NDEBUG
builds (README recommends RelWithDebInfo, which defines NDEBUG).

**Zero overhead when off.** All LOG_ELAPSED-only code lives inside
`#ifdef LOG_ELAPSED` blocks grouped per logical region (struct fields,
open path, enter handler, exit handler, close path). With
`LOG_ELAPSED=OFF` none of `<chrono>`, `<sys/stat.h>`, the patch_fd, the
per-frame arrays, the cursor, or the pwrite() call compiles. The
binary is byte-identical to today's default.

**Shutdown race.** `trace_shutdown` does NOT close `patch_fd` (or `fp`) for
other threads — see the top-level shutdown-coordination note in the
Trace File Lifecycle section. A worker already past the `get_thread_fp()`
nullptr check and mid-pwrite when shutdown runs simply completes its
pwrite against a still-valid fd; the duration lands normally. The only
visible shutdown artifact remains the torn-final-line case for `fprintf`.

### Indentation / Nesting Depth

A `thread_local` `current_stack_depth` counter in `trace.cpp`:
- Increments on each successfully resolved function entry
- Decrements on function exit (only if the entry was resolved)
- A fixed-size `frame_resolved_stack[2048]` array (also `thread_local`) tracks per-frame
  whether the entry was logged (file open AND resolution succeeded): every
  `__cyg_profile_func_enter` pushes and every `__cyg_profile_func_exit` pops
  **unconditionally** — pairing depends only on call structure, never on whether the
  trace file happens to be available. This keeps enter/exit matched across the
  trace-ready and shutdown boundaries (a frame entered before shutdown and exited
  after it still pops its own slot, so LOG_ELAPSED patches the right line). Depth is
  only adjusted for logged frames.
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

With `LOG_ELAPSED` defined, a 12-byte fixed-width duration field sits between
the timestamp and the rest (compatible with `LOG_ADDR`):
```
[DD-MM-YYYY HH:MM:SS.mmm] [   1.234ms] |_ functionName  (called from: filename.cpp:42)
[DD-MM-YYYY HH:MM:SS.mmm] [   1.234ms] addr: [0x00007fff12345678] |_ functionName  (called from: ...)
```

## File Structure

```
call-stack-logger/
|-- CMakeLists.txt              # Root CMake config (project definition, run target)
|-- Dockerfile                  # Ubuntu 24.04 build environment
|-- docker-compose.yml          # Build, test, coverage services
|-- README.md                   # Project overview, build instructions
|-- CONTRIBUTING.md             # Contribution guidelines
|-- CONTRIBUTOR_AGREEMENT.md    # CLA for contributors
|-- LICENSE                     # GNU AGPLv3 full text
|-- LICENSE_COMMERCIAL          # Commercial license placeholder
|-- .clang-format               # Code formatting rules (WebKit-based, 110 col limit)
|-- .gitignore                  # Ignores build artifacts, IDE files, *.out
|-- include/
|   |-- callStack.h             # bfdResolver struct, get_call_stack(), resolve() API
|   |-- durationFormat.h        # utils::format_duration_12chars (LOG_ELAPSED helper)
|   |-- format.h                # utils::format() - formats ResolvedFrame into string
|   |-- prettyTime.h            # utils::pretty_time() + PRETTY_TIME_LENGTH constant
|   |-- traceFilePath.h         # utils::resolve_base_trace_path + build_trace_filename
|   |-- types.h                 # ResolvedFrame struct definition
|   |-- unwinder.h              # FrameUnwinder template, Callback, unwind_nth_frame()
|-- src/
|   |-- CMakeLists.txt          # Build config (flags, std lib exclusion, library + executable)
|   |-- callStack.cpp           # Core implementation: BFD loading, symbol resolution
|   |-- trace.cpp               # __cyg_profile_func_enter/exit, trace file I/O
|   |-- main.cpp                # Demo program exercising various C++ features
|-- tests/                      # Unit and integration tests (BUILD_TESTS=ON)
|   |-- CMakeLists.txt          # FetchContent for Google Test, add subdirs
|   |-- unit/
|   |   |-- CMakeLists.txt      # Unit test executable (no instrumentation)
|   |   |-- test_duration_format.cpp # Tests for utils::format_duration_12chars()
|   |   |-- test_format.cpp     # Tests for utils::format()
|   |   |-- test_pretty_time.cpp # Tests for utils::pretty_time() and to_ms()
|   |   |-- test_trace_file_path.cpp # Tests for resolve_base_trace_path + build_trace_filename
|   |-- integration/
|       |-- CMakeLists.txt      # Traced programs + integration test runner
|       |-- traced_program.cpp  # Instrumented single-threaded program for testing
|       |-- threaded_traced_program.cpp # Instrumented multi-threaded program (per-thread files)
|       |-- log_elapsed_traced_program.cpp # Instrumented program with usleep() sentinels for LOG_ELAPSED tests
|       |-- callstack_api_program.cpp # Non-instrumented; exercises get_call_stack() API
|       |-- stripped_caller_lib.cpp # Shared-lib fixture, stripped of symtab/debug info post-build
|       |-- stripped_caller_program.cpp # Instrumented; callback invoked from the stripped lib
|       |-- test_integration.cpp # All integration tests (single/multi-threaded + API)
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
**Note:** Uses `localtime_r()` (thread-safe POSIX variant). Also exposes
`utils::PRETTY_TIME_LENGTH = 23` — the guaranteed output length, depended on by
LOG_ELAPSED to derive the placeholder byte offset without magic numbers.
`PrettyTimeTest.LengthMatchesConstant` enforces the constant matches actual output.

### `include/durationFormat.h`
`utils::format_duration_12chars(uint64_t ns, char out[13])` renders a nanosecond
duration into a fixed 12-byte field with auto-scaled SI units (`[ 123.456ns]`,
`[ 123.456us]`, `[ 123.456ms]`, `[ 123.456s ]`, saturating at `[  >999.9s ]`).
Also exposes the `DURATION_PLACEHOLDER` ("[  pending ]") and `DURATION_SATURATION`
constants. The 12-byte width is a hard invariant — `static_assert`s and the
`DurationFormatTest` suite enforce every code path matches it. Used only when
LOG_ELAPSED is enabled.

### `include/traceFilePath.h`
Two pure `NO_INSTRUMENT inline` helpers used by `trace.cpp` to compute per-thread trace
file paths:
- `utils::resolve_base_trace_path(const char* env_value)` — returns `env_value` if
  non-null/non-empty, else `DEFAULT_TRACE_FILENAME` ("trace.out")
- `utils::build_trace_filename(base, is_main, tid)` — returns `base` unchanged for the
  main thread, or `base + "_tid_<tid>"` for worker threads

Pure: no globals, no I/O, no syscalls — unit-tested in `test_trace_file_path.cpp`.

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
- `trace_begin()` - Constructor: opens main thread's trace file, writes run separator,
  registers `trace_shutdown` via `atexit()`, warns on failure
- `trace_shutdown()` - atexit handler: flushes every registered thread's file, clears
  the registry, sets `shutdown_complete` (no `trace_end` destructor exists — it was
  removed as dead code; see the comment in trace.cpp)
- `__cyg_profile_func_enter()` - Resolves, logs function entry, pushes to resolution stack
- `__cyg_profile_func_exit()` - Pops resolution stack, decrements depth if frame was logged
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

### CMake

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
| `LOG_ELAPSED` | Record per-function duration via in-place pwrite() patching of a 12-byte placeholder spliced after the timestamp. See "Per-function timing" below. |
| `DISABLE_INSTRUMENTATION` | Compile without any instrumentation hooks |

### Link Dependencies

| Library | Purpose |
|---------|---------|
| `-ldl` | Dynamic linking (`dladdr`) |
| `-lbfd` | Binary File Descriptor (symbol/line resolution) |
| `-lzstd` | Only when a configure-time probe detects static libbfd needs it (binutils ≥ 2.46, e.g. Fedora 41+ — the distro `-lbfd` linker script omits it) |

The probe lives in the root `CMakeLists.txt` and produces `CSLG_BFD_LINK_LIBS`
(`dl bfd` or `dl bfd zstd`), consumed by `src/CMakeLists.txt` and by the
per-flag library variants in `tests/integration/CMakeLists.txt`.

### System Requirement

- **GNU Binutils dev package:** `sudo apt-get install binutils-dev` (provides `libbfd`);
  on Fedora: `sudo dnf install binutils-devel libzstd-devel` (see Link Dependencies)

### Library Targets

The build produces three CMake targets:

- `callstacklogger` (static library, plus alias `callstacklogger::callstacklogger`) —
  the library itself (`callStack.cpp` + `trace.cpp`). As an INTERFACE it propagates
  `-g`, `-rdynamic`, the include path, and `-ldl -lbfd` to consumers. Link plain
  `callstacklogger` when you want the on-demand `get_call_stack()` API without
  per-call `-finstrument-functions` hooks.
- `callstacklogger_instrumented` (INTERFACE, plus alias `callstacklogger::instrumented`) —
  bundles `INSTRUMENT_FLAGS` (compile-time `-finstrument-functions` plus GCC's
  exclude-file-list) and links `callstacklogger`. **The single target users link to get
  full tracing.**
- `runDemo` (executable) — only built when the project is top-level (`CSLG_TOP_LEVEL`).
  Skipped when cslg is pulled in via `add_subdirectory` or FetchContent so user projects
  don't get it built as a side effect.

External users integrate by `FetchContent_MakeAvailable` or `add_subdirectory` and then:
```cmake
target_link_libraries(my_app PRIVATE callstacklogger::instrumented)
```
No per-target `target_compile_options` or `-rdynamic` boilerplate needed — everything
propagates through the INTERFACE. See README "Integrating into your own project".

## Testing

Tests are built when `BUILD_TESTS=ON` is passed to CMake. Google Test is fetched via
FetchContent (downloaded once, cached for offline use).

### Unit Tests (`tests/unit/`)

Test pure/deterministic functions from the include headers:
- `test_format.cpp` — tree indentation, address formatting, line numbers, buffer handling
- `test_pretty_time.cpp` — timestamp format, length, milliseconds, `to_ms()` conversion;
  `LengthMatchesConstant` enforces `pretty_time().size() == utils::PRETTY_TIME_LENGTH`
  (LOG_ELAPSED depends on this for byte-offset derivation)
- `test_trace_file_path.cpp` — `utils::resolve_base_trace_path()` (env var handling with
  null/empty/absolute/relative/special-char paths) and `utils::build_trace_filename()`
  (main vs worker thread suffix, small and LONG_MAX TIDs)
- `test_duration_format.cpp` — exhaustive coverage of `utils::format_duration_12chars()`:
  zero, ns / us / ms / s ranges and boundaries, saturation at 1000s and UINT64_MAX,
  framing characters, fixed 12-byte width invariant, buffer-overflow canary

### Integration Tests (`tests/integration/`)

- `traced_program.cpp` — small instrumented program with varied call patterns (free functions,
  static methods, templates, constructors, inline functions, STL usage via `func_with_stl()`)
- `test_integration.cpp` — executes `traced_test_program`, parses trace output, verifies:
  function names resolved, nesting depth correct, caller info present, timestamp format,
  run separator, CSLG_OUTPUT_FILE redirection, std library functions excluded,
  exact trace line count (catches std library pollution regressions)
- Built as six targets: `traced_test_program` (compiled WITH `INSTRUMENT_FLAGS`),
  `noninstrumented_test_program` (compiled WITHOUT — simulates
  `DISABLE_INSTRUMENTATION`), `threaded_traced_test_program` (spawns 4 worker
  threads via `std::thread`, exercises per-thread trace files),
  `callstack_api_program` (compiled WITHOUT `INSTRUMENT_FLAGS`; exercises the
  public on-demand `instrumentation::get_call_stack()` API by walking a known
  nested chain and printing each resolved frame), `stripped_caller` (shared
  library, `strip --strip-all`-ed post-build), and `stripped_caller_program`
  (instrumented; its callback is invoked from a file-local function inside the
  stripped library).
  The `DisableInstrumentationTest.NoTraceOutputWithoutInstrumentation` test runs
  the non-instrumented version and verifies zero trace entries are produced.
- `CallStackApiTest.GetCallStackResolvesAncestors` — runs `callstack_api_program`,
  captures stdout, verifies the on-demand stack contains the expected ancestor
  functions (`print_stack_from_leaf → callstack_mid → callstack_top → main`) in
  innermost-first order.
- `BadOutputPathTest.OpenFailureIsNonFatalAndWarns` — runs `traced_test_program`
  with `CSLG_OUTPUT_FILE` pointing to a non-existent directory, captures stderr,
  verifies the program exits 0 (graceful degradation) and emits the documented
  `[call-stack-logger] WARNING` with the attempted path.
- `StrippedCallerTest.CallerInStrippedLibraryDoesNotHang` — regression test for
  the `resolve_filename_and_line()` infinite loop: the instrumented callback's
  caller address lies in a file-local function of the stripped
  `libstripped_caller.so`, so `dladdr` yields no symbol and
  `bfd_find_nearest_line` fails; the resolver must return the `<bfd_error>`
  fallback instead of spinning forever while holding `s_bfd_mutex`. The program
  runs under `timeout 10` (with `DEBUGINFOD_URLS` cleared so libbfd cannot
  fetch debug info from the network), so a regression fails fast with exit
  code 124 instead of hanging the suite.
- `LogAddrFlagTest` (2 tests) and `LogNotDemangledFlagTest` (1 test) exercise the
  build-time CMake options `-DLOG_ADDR=ON` and `-DLOG_NOT_DEMANGLED=ON`. Each flag
  has its own library variant in `tests/integration/CMakeLists.txt` —
  `callstacklogger_log_addr` / `callstacklogger_log_not_demangled` — built from the
  same sources with the corresponding macro defined, and a `traced_test_program_<variant>`
  that links it. `LogAddrFlagTest.AddressPrefixAppearsInTrace` asserts every entry in
  the variant's trace has the `addr: [0x<hex>]` prefix, while
  `LogAddrFlagTest.NoAddressPrefixWithoutFlag` is a negative test on the default
  build to guard against the macro accidentally becoming always-on.
  `LogNotDemangledFlagTest.ProducesNormalTraceOutput` is a smoke test only — it
  verifies the macro wires through and produces a valid trace, since the actual
  differential behavior (logging frames where `dladdr` returns `dli_sname == nullptr`)
  is hard to trigger deterministically without a stripped/JITted callee.
- `threaded_traced_program.cpp` — spawns 4 worker threads (each calling a
  `worker_top → worker_mid → worker_leaf` chain), then runs `main_only_post_join()`
  on the main thread after join. Prints `MAIN_TID=<n>` to stdout so the test fixture
  can verify the main file's header thread ID.
- `ThreadedIntegrationTest` fixture (11 tests) uses `mkdtemp()` for a unique temp
  directory per test, runs the threaded program with `CSLG_OUTPUT_FILE=<tmpdir>/…`,
  enumerates all produced files, and verifies: main file exists, N=4 worker files
  exist with distinct numeric TIDs in filenames, every file has the run-separator
  header with the "thread ID: <n>" suffix, main's header contains the MAIN_TID from
  stdout, each worker's header TID matches its filename suffix, worker files contain
  the full `worker_top/mid/leaf` chain, main-only post-join calls appear only in
  the main file (no cross-contamination), no std library pollution in worker files
  (important on Clang — `std::thread` internals would explode the trace without
  the runtime filter), all worker files have identical function entry counts
  (proves deterministic per-thread traces).
- `LogElapsedFlagTest` fixture (5 tests) drives `traced_test_program_log_elapsed`
  built from `log_elapsed_traced_program.cpp` (a dedicated driver with an
  `elapsed_outer → elapsed_middle → elapsed_inner` chain and a `usleep(10000)`
  sentinel inside `elapsed_inner`). Verifies: every entry line carries a 12-byte
  duration field, no `[  pending ]` placeholders remain after clean exit, parent
  durations ≥ child durations along the nested chain (proves per-frame indexing
  of `frame_enter_time` / `frame_placeholder_offset` is correct under LIFO frame
  stacking), `elapsed_inner` reports ≥10 ms (the usleep), and every line parses
  cleanly against a strict `[ts] [dur] … (called from: file:line)` regex (proves
  pwrite never spills past its 12-byte window).
  `LogElapsedDefaultBuildTest.NoDurationFieldWithoutFlag` is a negative test
  on the default build (skipped when the build is configured with
  `-DLOG_ELAPSED=ON`, via `CSLG_DEFAULT_HAS_LOG_ELAPSED`).
- `LogElapsedCombinedFlagsTest` fixture (4 tests) runs the all-three-flags
  variant `traced_test_program_log_elapsed_addr_not_demangled`. Asserts the
  ordering "timestamp → duration → addr" via regex, the tree column stays
  byte-aligned across same-depth lines (proves the fixed-width LOG_ELAPSED
  prefix preserves alignment under the LOG_ADDR layout), no pending leftovers,
  and the `addr: [0x...]` column never gets corrupted by pwrite (catches any
  byte-stomping past the 12-byte field, since the addr column starts ~13 bytes
  after the placeholder).

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
lcov --capture --directory build --output-file coverage.info --ignore-errors mismatch,negative
lcov --remove coverage.info '/usr/*' '*/tests/*' '*/_deps/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage-report
```

Notes:
- `COVERAGE=ON` compiles with `--coverage -fprofile-update=atomic`. The atomic
  counter updates are load-bearing: the multi-threaded `threaded_traced_test_program`
  runs the instrumented library (`trace.cpp`) on 4 worker threads that would otherwise
  race on the default non-atomic gcov counters. A lost update can drive a counter
  negative, and lcov 2.x's `geninfo` treats a negative count as a *fatal* error
  (`Unexpected negative count '-1'`), producing a nondeterministic coverage-job failure.
  Atomic updates remove the race; the `negative` entry in `--ignore-errors` is a
  belt-and-suspenders tolerance for any residual artifact.
- `--ignore-errors mismatch` bypasses lcov 2.0 strict checks that fail on gtest code.
- `--no-external` is NOT used: in lcov 2.0 it uses `--directory` as the base for
  "external" checks, which would wrongly exclude our `src/` files (outside `build/`).
  Instead, explicitly filter system headers via `--remove '/usr/*'`.

### Sanitizers

The `SANITIZE` CMake option applies ASan/UBSan/TSan to every cslg-owned target
(library + demo + test programs). The flags are attached as **PRIVATE** compile and
link options via the `cslg_apply_sanitizers(target)` helper defined at the root
`CMakeLists.txt` — never as INTERFACE on `callstacklogger`. This is deliberate:
external users who integrate via `add_subdirectory` / FetchContent must not inherit
sanitizer flags, otherwise their production builds would get sanitized at link time.

Accepted values: `address`, `undefined`, `address+undefined`, `thread`. ASan and TSan
are mutually exclusive. MSan is not supported (needs a fully MSan-instrumented
libstdc++/libc). Invocation:
```bash
cmake -B build-asan -DBUILD_TESTS=ON -DSANITIZE=address+undefined
cmake -B build-tsan -DBUILD_TESTS=ON -DSANITIZE=thread
```

LSan reports from inside `libbfd` (GNU binutils keeps static symbol-table / object-file
caches live to program exit — not a leak in cslg's code) are silenced via
`tests/lsan-suppressions.txt`. Pass it with
`LSAN_OPTIONS=suppressions=.../tests/lsan-suppressions.txt`. Our own allocations are
still caught. The Clang builds need `libclang-rt-18-dev` (added to the Dockerfile) —
GCC ships its sanitizer runtimes with `g++`.

docker compose services: `sanitize-asan` and `sanitize-tsan` (GCC), plus
`sanitize-asan-clang` and `sanitize-tsan-clang` (Clang). All run the full test suite
under the respective sanitizer.

GitHub Actions CI runs the two GCC sanitizer services on every push and PR:
`sanitize-asan` (ASan + UBSan + LSan) and `sanitize-tsan` (TSan). Clang sanitizer
runs are not part of CI to keep the maintenance surface small — they remain
available locally via docker-compose.

## CI/CD

GitHub Actions (`.github/workflows/ci.yml`) runs on push/PR to `master` with four jobs:
- **gcc**: builds with `BUILD_TESTS=ON` and `COVERAGE=ON`, runs tests via `ctest`, generates and uploads the lcov HTML coverage report.
- **clang**: builds with `-DCMAKE_CXX_COMPILER=clang++`, runs tests.
- **sanitize-asan**: builds with `SANITIZE=address+undefined`, runs tests under ASan + UBSan + LSan (libbfd suppression via `${{ github.workspace }}/tests/lsan-suppressions.txt`).
- **sanitize-tsan**: builds with `SANITIZE=thread`, runs tests under TSan.

Clang sanitizer runs are intentionally NOT in CI — Clang's LSan drifts across toolchain versions and would add maintenance noise. Available locally via docker-compose.

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
   static destructors run. The enter hook is additionally an **exception barrier**: its
   resolve/format/log work runs inside `try/catch(...)` with state mutations placed after
   the last throwing operation, so an OOM-time `bad_alloc` can never propagate out of the
   hook into the traced program — the frame just goes untraced and pairing stays intact
   (the exit hook performs no throwing operations and needs no barrier).
2. **Full multi-threaded support:** Each thread writes to its own independent trace
   file. Main → `CSLG_OUTPUT_FILE` (or `trace.out`); workers → `<base>_tid_<gettid>`
   where `<gettid>` is the Linux kernel TID from `syscall(SYS_gettid)` (cached per
   thread in `t_state.cached_tid` to avoid a syscall per trace call).
   Per-thread state lives in the `PerThreadState` thread_local struct; per-thread
   `FILE*` lives in an RAII `PerThreadTraceFile` member whose destructor runs on
   thread exit. **Zero mutex on the hot write path** — each thread writes to its own
   FILE*. `open_files_mutex` is taken only during open (once per thread) and during
   shutdown (to fflush the registry). `s_bfd_mutex` is still held for the entire
   `resolve()` call because BFD is not thread-safe — but file I/O is fully parallel.
   **Shutdown does NOT close other threads' descriptors** — it only fflushes — to
   avoid UAF on the stdio FILE struct and fd-number-reuse for patch_fd (see the
   Trace File Lifecycle section above). Per-thread destructors close on thread exit.
   Worst-case shutdown artifact is one torn final line per thread. `fork()` is
   not supported — child inherits parent's thread_local FILE* pointers.
   `localtime_r` replaces `std::localtime` for thread-safe timestamp formatting.
3. **Frame 6 constant:** The unwinder hard-codes frame depth 6 - must be recalculated if the
   call chain between `__cyg_profile_func_enter` and `unwind_nth_frame` changes.
   Only functions still on the stack during the unwind count: helpers that the enter
   hook calls and that return before `resolve()` runs (e.g. `get_thread_fp()`) can
   never shift the frame numbers, inlined or not.
4. **Append mode:** Trace output is opened with `O_APPEND | O_NOFOLLOW` - multiple runs
   accumulate, separated by timestamped headers; output is line-buffered (`_IOLBF`)
   with `0600` permissions (owner read/write only). Note `O_NOFOLLOW` rejects a
   symlink only as the FINAL path component — intermediate directories are still
   followed, so the trace path should point at a trusted directory, and the library
   must not be used in setuid/setgid binaries (the path comes from the environment).
5. **Configurable output:** Set `CSLG_OUTPUT_FILE` environment variable to redirect trace
   output to a custom path (defaults to `"trace.out"`)
6. **Performance overhead:** The first call for each callee/call-site address triggers
   full symbol resolution via BFD; repeat calls hit the per-address memoization caches
   (see Symbol Resolution Pipeline) and cost a hash lookup plus the file write. Still
   a debugging/tracing tool, not for production use. `format()` uses `snprintf` with a
   stack buffer to avoid per-call heap allocation.
7. **Header-only utilities:** `format.h`, `prettyTime.h`, `unwinder.h`,
   `durationFormat.h` contain inline implementations in headers (definitions in
   headers, not just declarations).
8. **Per-function timing (`LOG_ELAPSED`)**: opt-in. Each thread opens a SECOND
   non-O_APPEND fd to its trace file (reopened via `/proc/self/fd/<fd>` so it is
   pinned to the same inode as the first fd) solely so `pwrite()` honors explicit
   byte offsets (Linux silently redirects pwrite to EOF when the fd has O_APPEND).
   Enter handler writes a 12-byte `[  pending ]` placeholder spliced after the
   timestamp; exit handler patches it in place with the SI-auto-scaled duration.
   Placeholder offset derived from `utils::PRETTY_TIME_LENGTH` (no magic
   numbers); a unit test plus an always-on runtime check in `trace_begin()`
   (active in NDEBUG builds too) guard against format-string drift. Crash-safe: lines that don't get patched stay as
   `[  pending ]`, identifying which frames were active at the crash. Zero
   overhead when `LOG_ELAPSED=OFF` — all code is `#ifdef`-guarded, the binary
   is byte-identical to a build without the option. See "Per-Function Timing
   (LOG_ELAPSED)" in the Architecture section above.
9. **Exceptions under Clang (known limitation):** Clang's `-finstrument-functions`
   does not emit `__cyg_profile_func_exit` on the exception-unwind path — frames
   unwound by a throw never run their exit hook. GCC emits the exit call on the
   exceptional path too (like a cleanup), so enter/exit stay paired (verified
   empirically with a minimal probe at -O0 and -O2 on both compilers). Under
   Clang, a caught exception that unwinds through instrumented frames leaves one
   stale entry per unwound frame on the per-thread frame stack:
   `current_stack_depth` and tree indentation drift deeper for the rest of the
   thread, and with `LOG_ELAPSED` subsequent exits pop the stale slots, so
   durations get patched onto the wrong (already-unwound) lines while the frames
   that keep running stay `[  pending ]`. Documented in README's
   "Compiler-specific instrumentation" section; prefer GCC for tracing
   exception-heavy code. A code-level self-heal (recording the callee address
   per slot and popping stale entries on mismatch) was considered and rejected
   for now: it cannot disambiguate recursive frames (same callee address at
   several depths).
10. **Signal handlers must not be instrumented (known limitation):** the hooks
   allocate, lock `s_bfd_mutex`, and use stdio — none async-signal-safe. A signal
   interrupting a thread mid-`malloc` whose handler runs instrumented functions
   re-enters the allocator on the same thread and can deadlock. (The
   `in_instrumentation` guard incidentally covers signals landing *inside* the
   resolve pipeline, but not the interrupted-allocator case.) Signal handlers
   should live in a TU compiled without `-finstrument-functions` or carry
   `NO_INSTRUMENT`. Documented in README's Thread Safety section.

## Use Cases (from the article)

The tool is designed for scenarios where traditional debugging is impractical:
- Problem reproducible only on customer/remote infrastructure
- Lacking necessary test setup to reproduce locally
- Bug lies in third-party library code without source access
- Need to understand flow of execution in large, unfamiliar codebases
- Step-through debugging too slow or not available

## Git History Summary

The project evolved through these milestones (earliest first):
1. Initial commit with basic class/function name printing
2. Added standard library exclusion from instrumentation
3. Added filename and line number resolution via BFD
4. Added unwinder for correct caller location resolution
5. Added NO_INSTRUMENT macro, formatting, timestamps
6. Added optional LOG_ADDR and LOG_NOT_DEMANGLED flags
7. Migrated to CMake build system (legacy Makefiles kept for a while, since removed —
   they predated split compilation, LOG_ELAPSED, and the zstd link probe)
8. Added documentation, article link, licensing, contribution guidelines
