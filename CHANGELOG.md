# Changelog

## v1.0.0

The first numbered release. The library's behavior is the one documented in
the README; this entry lists what changed against the last unnumbered state
of `master`.

### Symbol resolution without GPL code

- Addresses are resolved with [libbacktrace](https://github.com/ianlancetaylor/libbacktrace)
  (BSD-3-Clause) instead of libbfd from GNU binutils (GPL-3.0-or-later). A
  program built with `callstacklogger::instrumented` no longer contains GPL
  code, so a diagnostic build can ship under the commercial license. The
  dependency tables before and after, with every license, are in
  `THIRD_PARTY.md`.
- `binutils-dev` / `binutils-devel` / `libzstd-devel` are no longer needed.
  libbacktrace's sources are downloaded at configure time from a pinned
  commit (SHA-256 checked) and compiled into the library; set
  `FETCHCONTENT_SOURCE_DIR_CSLG_LIBBACKTRACE` to a local copy for offline builds.
- The public header `include/callStack.h` includes no third-party header any
  more (it included `bfd.h`). The resolver struct it declares is now called
  `addressResolver` (was `bfdResolver`); the free functions of the API are
  unchanged.
- Trace output is unchanged wherever a name or a line could be resolved. The
  degraded texts changed: `<bfd_error>` is now `<unknown function>`, a name
  that only the dynamic symbol table knows is printed without the
  ` <bfd_error>` suffix, and `<no sections in object file>` /
  `<not sectioned address>` no longer occur. Separate debug files
  (`.gnu_debuglink`, build IDs under `/usr/lib/debug`), zlib- and
  zstd-compressed debug sections and MiniDebugInfo (`.gnu_debugdata`) are read.
- The resolver opens only files the kernel names for a mapping: a plugin
  deleted after loading is reported as `<could not open object file>` even
  when a file has been created under the name `/proc/self/maps` shows for it
  (`<path> (deleted)`), and the vdso, which dladdr() names
  `linux-vdso.so.1`, is never looked up in the current directory. Earlier
  releases opened both names and read whatever file they found.
- A truncated object file (a half-copied plugin, for example) is rejected and
  reported as `<could not open object file>`; libbacktrace alone would map
  the missing bytes and fault when reading them. The same goes for object
  files whose headers point outside the file (a section index out of range,
  a note larger than its section); a truncated separate debug file or dwz alt
  file is skipped.
- Only regular files are opened when looking for separate debug files and dwz
  alt files. A FIFO under the name of a debug file hung earlier releases at
  the first traced call into the object: opening a FIFO waits for a writer.
- Under Clang, `callstacklogger::instrumented` no longer adds `-gdwarf-4`
  with `LOG_EXCEPTIONS`: libbacktrace walks Clang's DWARF 5 inline chains, so
  the frame reconciliation of inlined frames is exact with Clang's default
  debug format.

### On-demand API

- `get_call_stack()` names each frame after the function that owns it, the
  outermost function of the inline chain at the frame's return address (minus
  one byte, into the call). Earlier versions took the innermost function
  there, which at `-O2` is often a helper inlined right after the call, so a
  frame could read as `__normal_iterator::__normal_iterator(...)` or
  `std::basic_ios::basic_ios()` instead of the function that made the call.
  The hooks' naming is unchanged.

### Linking

- The functions of the libbacktrace copy inside the library carry a prefix of
  their own (`cslg_vendored_backtrace_*`), so a program that links another
  libbacktrace statically (its own copy, Boost.Stacktrace's libbacktrace
  backend) links and traces as before.
- Programs export only the symbols that must be visible in the dynamic symbol
  table — the two hooks, for `dlopen()`ed plugins, and with `LOG_EXCEPTIONS`
  the five interposed runtime entry points — through
  `--export-dynamic-symbol`, instead of every symbol through `-rdynamic`.
  `-rdynamic` remains the fallback for linkers without that option (GNU ld
  before binutils 2.35) and the choice behind the new CMake option
  `CSLG_EXPORT_ALL_SYMBOLS=ON`. Visible difference: an executable stripped
  of its symbol table without a separate debug file shows no names at all
  with the narrow export (it showed names without lines before); keep a
  `.gnu_debuglink` debug file next to a stripped binary.
- New CMake helper `cslg_keep_debug_info(<target> [INSTALL_DESTINATION <dir>])`:
  after each link it writes `<target>.debug` and adds a `.gnu_debuglink` to the
  binary, so that a later `strip --strip-all` keeps names and lines resolvable
  as long as the debug file ships next to the binary or under
  `/usr/lib/debug`.
- The library prints one warning on `stderr` when the executable has neither
  a symbol table nor debug information, because none of its functions can be
  named then and the trace would otherwise stay empty without explanation.
  MiniDebugInfo (`.gnu_debugdata`, what Fedora's stripped binaries keep)
  counts as a symbol table.
- `DISABLE_INSTRUMENTATION=ON` now makes `callstacklogger::instrumented`
  contribute nothing to a program: no `-finstrument-functions`, no `-g`, no
  exported symbols, no library. A program that calls `get_call_stack()` in
  such a build links `callstacklogger::callstacklogger` explicitly. The test
  suite proves the target inert by comparing a program built with it against
  one built without (`tests/disabled/`).
- The static library is compiled position-independent, so it links into
  position-independent executables on hosts whose compiler defaults to
  fixed-address ones.

### Tests, benchmark, CI

- New integration tests: a separate debug file made with `objcopy` in the
  build, zlib- and zstd-compressed debug sections, a plugin deleted after
  loading, a truncated plugin, malformed plugins, a truncated alt file, FIFOs
  under the names of debug files, addresses outside every object, PIE and
  non-PIE executables, a stripped executable with and without a debug file
  and with MiniDebugInfo, a program that defines libbacktrace's functions
  itself.
- `bench/`: a benchmark of the warm per-call cost, the cold resolution cost
  and the resident memory, buildable against any checkout
  (`bench/run_bench.sh`), with a `bench` target behind `BUILD_BENCH=ON`.
- CI and the Docker image install no `binutils-dev`; the LeakSanitizer
  suppression file for libbfd is gone (the sanitizer jobs run without
  suppressions); the Docker image installs `ca-certificates` for the
  configure-time downloads.
