/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#include "callStack.h"
#include "format.h"
#include "prettyTime.h"
#include "traceFilePath.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <stdio.h>
#include <string>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#ifdef LOG_ELAPSED
    #include "durationFormat.h"
    #include <chrono>
#endif

// clang-format off
#ifndef DISABLE_INSTRUMENTATION

// Maximum tracked call depth for the frame resolution stack. Each entry is 1 byte (bool);
// with LOG_ELAPSED, two parallel per-frame arrays (time_point + off_t, 16 bytes) join it,
// so the per-thread cost is MAX_TRACE_DEPTH bytes without LOG_ELAPSED and ~17x that
// (~34 KB at 2048) with it. If call depth exceeds this limit, an overflow counter
// prevents stack desynchronization. The value can be increased if needed.
static constexpr int MAX_TRACE_DEPTH = 2048;

// Per-thread RAII wrapper around the thread's FILE*. On thread exit, the destructor
// closes the file and removes this instance from the global registry. Coordinates
// with trace_shutdown() via g_trace().open_files_mutex.
//
// `fp` is std::atomic because trace_shutdown() on another thread READS it through
// the open_files registry (to fflush) while the owning thread accesses it lock-free
// on the hot path. All writes (store on open, null on thread exit) happen on the
// owning thread, ordered against shutdown by open_files_mutex — shutdown itself
// deliberately never closes or nulls other threads' fps (see trace_shutdown()).
// The atomic keeps the cross-thread read formally race-free per the C++ memory
// model. Relaxed ordering is sufficient: no data is published through fp, just the
// pointer value itself. Codegen on x86_64 / ARM64 is a single mov/LDR — no
// measurable hot-path overhead.
struct PerThreadTraceFile {
    std::atomic<FILE*> fp{nullptr};
    bool open_attempted = false;   // avoid retry after open failure
    bool registered = false;       // set true once added to g_trace().open_files

#ifdef LOG_ELAPSED
    // Second file descriptor to the same trace file, opened WITHOUT O_APPEND so
    // pwrite() can patch the fixed-width duration placeholder at explicit byte
    // offsets. fp (above) still uses O_APPEND for sequential line writes —
    // the two fds share the kernel inode; writes never overlap in byte range
    // (fp writes NEW bytes beyond EOF, patch_fd rewrites EXISTING placeholder
    // bytes). Atomic for consistency with fp: all accesses are owner-thread or
    // mutex-ordered (trace_shutdown deliberately never touches patch_fd), so the
    // atomic is formal belt-and-suspenders, not a contention point.
    std::atomic<int> patch_fd{-1};
#endif

    NO_INSTRUMENT PerThreadTraceFile() = default;
    NO_INSTRUMENT ~PerThreadTraceFile();   // defined after TraceGlobals

    PerThreadTraceFile(const PerThreadTraceFile&) = delete;
    PerThreadTraceFile& operator=(const PerThreadTraceFile&) = delete;
};

// All per-thread state bundled in one struct for readability.
//
// IMPORTANT member order: the re-entrancy guard `in_instrumentation` is declared FIRST
// so it is destroyed LAST (members are destroyed in reverse declaration order). That
// keeps the guard alive during destruction of `trace_file`, whose destructor may be
// instrumented by Clang.
struct PerThreadState {
    bool in_instrumentation = false;
    int current_stack_depth = -1;
    bool frame_resolved_stack[MAX_TRACE_DEPTH] = {};
    int frame_resolved_top = -1;
    int frame_overflow_count = 0;
    // Cached gettid() result (0 means not yet resolved). Avoids a syscall per trace call.
    pid_t cached_tid = 0;

#ifdef LOG_ELAPSED
    // Per-frame enter timestamp, used to compute elapsed duration on exit.
    // Indexed by the same counter as frame_resolved_stack[] — one slot per
    // instrumented-and-resolved frame currently on the stack.
    std::chrono::steady_clock::time_point frame_enter_time[MAX_TRACE_DEPTH] = {};

    // Byte offset (into this thread's trace file) of the "[  pending ]"
    // placeholder for each frame. On exit we pwrite the formatted duration at
    // this offset. Using an int64_t-compatible type so off_t's signedness is
    // explicit at the use site.
    off_t frame_placeholder_offset[MAX_TRACE_DEPTH] = {};

    // Running byte position for this thread's trace file. Seeded from the file's
    // end position when the file is opened (handles multi-run append where
    // earlier runs already wrote content + separator headers). Advanced by
    // exactly the number of bytes we wrote for every line — lets us compute
    // placeholder offsets without calling ftello() on the hot path.
    off_t cursor = 0;

    // True while `cursor` provably matches the file's real end position. Cleared
    // permanently (for this thread) the first time a line write fails (ENOSPC,
    // EIO, quota) or the initial lseek cannot determine EOF — after a failed
    // write the number of bytes that actually reached the file is unknown, so
    // every cursor-derived offset from then on would be a guess. Frames recorded
    // while the cursor was still valid keep their correct offsets and are still
    // patched; frames entered afterwards record a -1 offset sentinel and keep
    // their "[  pending ]" placeholder (the documented degraded mode) instead of
    // risking pwrite splicing duration bytes into the middle of other lines.
    bool cursor_valid = true;
#endif

    PerThreadTraceFile trace_file;
};

static thread_local PerThreadState t_state;

// Re-entrancy guard access for the public API entry points in callStack.cpp
// (get_call_stack(), instrumentation::resolve()). Those entry points run
// resolver code that holds s_bfd_mutex while executing std container/string
// template code. Under Clang, the COMDAT instantiations of those templates can
// be the copies compiled in the USER's instrumented TU (any TU that includes
// callStack.h emits them), so __cyg_profile_func_enter can fire in the middle
// of the resolver and re-lock s_bfd_mutex on the same thread — a guaranteed
// self-deadlock. The entry points set this per-thread guard for their whole
// duration so the hook no-ops, exactly as the hook does for its own pipeline.
// Save/restore semantics keep nesting correct (the enter hook already holds
// the guard when it calls instrumentation::resolve()).
namespace instrumentation {

NO_INSTRUMENT
bool enter_no_instrument_scope() {
    const bool prev = t_state.in_instrumentation;
    t_state.in_instrumentation = true;
    return prev;
}

NO_INSTRUMENT
void exit_no_instrument_scope(bool prev) {
    t_state.in_instrumentation = prev;
}

} // namespace instrumentation

// Process-wide globals bundled into one struct, behind a lazily-initialized (and
// deliberately leaked — see g_trace()) singleton accessor.
//
// Why a function-local static (not a plain static): TraceGlobals contains `std::string`
// and `std::vector`, which require dynamic initialization in C++17 (neither has a
// constexpr default ctor until C++20). A plain `static TraceGlobals g_trace` at file
// scope would not be fully constructed when `trace_begin()` runs — GCC's
// `__attribute__((constructor))` functions fire during early init, BEFORE C++ dynamic
// initialization of file-scope objects in the same TU completes. Accessing an
// unconstructed `std::string` here segfaults.
//
// Function-local statics are lazily initialized on first call and the C++11 standard
// guarantees thread-safe initialization (magic statics). That makes this safe from
// both `trace_begin()` pre-main and from worker threads that first call into the
// instrumentation later.
struct TraceGlobals {
    pid_t main_tid = -1;                          // captured in trace_begin()
    std::string base_path;                        // resolved once in trace_begin()

    // Protects open_files and the PerThreadTraceFile::fp null-transitions done inside
    // shutdown. Writers never touch this mutex — each thread writes to its own FILE*.
    std::mutex open_files_mutex;
    std::vector<PerThreadTraceFile*> open_files;

    std::atomic<bool> trace_ready{false};         // true after trace_begin() completes
    std::atomic<bool> shutdown_started{false};    // CAS guard for idempotent shutdown
    std::atomic<bool> shutdown_complete{false};   // writers observe this and stop
};

NO_INSTRUMENT
static TraceGlobals& g_trace() {
    // Deliberately heap-allocated and leaked (never destroyed). A thread that
    // exits during the tail of static destruction — after a plain function-local
    // static would already have been destroyed — still runs ~PerThreadTraceFile,
    // which locks open_files_mutex and touches open_files. With a normal Meyers
    // singleton that would operate on a destroyed mutex/vector (UB); the leaked
    // instance stays valid until the process image disappears and the kernel
    // reclaims everything. LSan treats a reachable global as live, so this does
    // not appear as a leak.
    static TraceGlobals* instance = new TraceGlobals;
    return *instance;
}

namespace {

// Cached per-thread Linux TID. Uses syscall(SYS_gettid) rather than gettid() for
// portability to glibc < 2.30. A TID is stable for the lifetime of a thread, so
// caching in t_state.cached_tid is safe.
NO_INSTRUMENT
pid_t current_tid() {
    if (t_state.cached_tid == 0) {
        t_state.cached_tid = static_cast<pid_t>(syscall(SYS_gettid));
    }
    return t_state.cached_tid;
}

// Writes the "=== New trace run: <timestamp>, thread ID: <tid> ===" header framed
// above and below by `=` lines of matching length. Called immediately after a file
// is opened (main thread in trace_begin, worker threads on lazy open).
NO_INSTRUMENT
void write_run_separator_header(FILE* fp, pid_t tid) {
    std::string middle = "=== New trace run: " + utils::pretty_time()
                       + ", thread ID: " + std::to_string(tid) + " ===";
    std::string frame(middle.size(), '=');
    // No fflush needed: setvbuf(_IOLBF) is already in effect, so each '\n' flushes.
    fprintf(fp, "\n%s\n%s\n%s\n", frame.c_str(), middle.c_str(), frame.c_str());
}

// Opens this thread's trace file (lazy for workers, eager for main via trace_begin).
// Idempotent: if open has already been attempted (success or failure), returns
// immediately. On success, installs the FILE*, switches to line-buffered mode, writes
// the run-separator header, and registers this PerThreadTraceFile* in the global
// open_files registry so trace_shutdown() can close it if the thread is still alive
// at program exit.
NO_INSTRUMENT
void open_this_thread_file(PerThreadState& self) {
    if (self.trace_file.open_attempted) {
        return;
    }
    self.trace_file.open_attempted = true;

    const pid_t tid = current_tid();
    TraceGlobals& g = g_trace();
    const bool is_main = (tid == g.main_tid);
    const std::string path = utils::build_trace_filename(g.base_path, is_main, tid);

    // Security-hardening flags: O_NOFOLLOW to prevent symlink attacks, 0600
    // permissions so traces (which can leak internal paths and function names)
    // are not world-readable, and O_CLOEXEC so an exec'd child of the traced
    // program does not inherit a writable descriptor to the trace file
    // (spawning subprocesses is normal in traced applications; only continuing
    // to trace after fork() is unsupported).
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "[call-stack-logger] WARNING: Could not open %s for writing\n",
                path.c_str());
        return;
    }
    FILE* fp = fdopen(fd, "a");
    if (fp == nullptr) {
        close(fd);
        fprintf(stderr, "[call-stack-logger] WARNING: fdopen failed for %s\n", path.c_str());
        return;
    }

#ifdef LOG_ELAPSED
    // Second fd to the same file, WITHOUT O_APPEND, reserved for pwrite()-patching
    // the fixed-width "[  pending ]" placeholders with real durations on exit. A
    // single O_APPEND fd would force every pwrite to EOF on Linux (documented in
    // pwrite(2)), so we need this separate handle. dup() can't provide it either:
    // duplicated descriptors share one open file description, so clearing
    // O_APPEND via fcntl would clear it for fp too.
    //
    // Reopen through /proc/self/fd/<fd> rather than through the path: the magic
    // symlink resolves to the already-open inode, so patch_fd is guaranteed to
    // reference the SAME file as fp even if the path was replaced between the
    // two opens (TOCTOU). /proc is already a hard dependency of this library
    // (get_argv0, /proc/self/exe). Note: no O_NOFOLLOW here — /proc/self/fd/N
    // is a kernel-controlled symlink that MUST be followed; the symlink-attack
    // concern the first open guards against does not apply to it. O_CLOEXEC
    // for the same reason as the first fd — this one is even more sensitive,
    // since without O_APPEND it can pwrite anywhere in the file.
    // Both fds share the kernel inode; writes never overlap in byte range —
    // fp only writes NEW bytes beyond EOF, pfd only rewrites EXISTING
    // placeholder bytes.
    char fd_path[32];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
    int pfd = open(fd_path, O_WRONLY | O_CLOEXEC);
    if (pfd < 0) {
        // Degrade rather than disable: the trace itself is still valuable without
        // durations. Keep tracing through fp; with pfd == -1 and cursor_valid
        // false, every frame records the -1 offset sentinel, the exit handler
        // skips its pwrite, and each line keeps the "[  pending ]" placeholder —
        // the same documented degraded mode as a mid-run write failure.
        // Realistic triggers: /proc not mounted (minimal chroot/container), fd
        // limit exhaustion.
        fprintf(stderr,
                "[call-stack-logger] WARNING: Could not reopen %s for duration patching "
                "— tracing continues, durations stay \"[  pending ]\"\n",
                path.c_str());
    }
#endif

    // Line-buffered mode: flushes after each '\n' (every trace line). Crash-safe
    // without per-call fflush overhead.
    setvbuf(fp, NULL, _IOLBF, 0);
    self.trace_file.fp.store(fp, std::memory_order_relaxed);
#ifdef LOG_ELAPSED
    self.trace_file.patch_fd.store(pfd, std::memory_order_relaxed);
#endif

    write_run_separator_header(fp, tid);

#ifdef LOG_ELAPSED
    if (pfd >= 0) {
        // Seed the per-thread byte cursor to the current EOF, which now reflects
        // everything written by prior runs (multi-run append) AND the separator
        // header we just wrote. Line-buffered stdio flushed the header on its final
        // '\n', so lseek on pfd observes the post-header size. SEEK_END on a
        // non-O_APPEND fd returns the file size without affecting fp's position.
        off_t eof = lseek(pfd, 0, SEEK_END);
        if (eof == static_cast<off_t>(-1)) {
            // Non-seekable target (pipe, some character devices): the placeholder
            // offsets would be guesses that could land inside the header. Disable
            // patching for this thread; placeholders stay "[  pending ]".
            self.cursor = 0;
            self.cursor_valid = false;
        } else {
            self.cursor = eof;
        }
    } else {
        // No patch fd (reopen failed above): offsets would be unusable anyway;
        // disable patching for this thread, tracing continues.
        self.cursor_valid = false;
    }
#endif

    // Register this file so trace_shutdown() can close it at program exit if the
    // thread is still alive. If shutdown has already completed (edge case: a new
    // thread starts tracing after atexit fired), close immediately instead of
    // leaking the fd.
    {
        std::lock_guard<std::mutex> lock(g.open_files_mutex);
        if (g.shutdown_complete.load(std::memory_order_relaxed)) {
            fclose(fp);
            self.trace_file.fp.store(nullptr, std::memory_order_relaxed);
#ifdef LOG_ELAPSED
            if (pfd >= 0) {
                close(pfd);
            }
            self.trace_file.patch_fd.store(-1, std::memory_order_relaxed);
#endif
            return;
        }
        g.open_files.push_back(&self.trace_file);
        self.trace_file.registered = true;
    }
}

// Returns the FILE* for the current thread, opening it on first use. Returns nullptr
// if trace is not ready, shutdown has completed, or the open failed earlier.
//
// Note on the frame-depth-6 constant in callStack.cpp: this helper CANNOT affect it,
// regardless of inlining — it returns before instrumentation::resolve() is invoked,
// so it is never on the stack while the unwinder walks the resolve pipeline. The
// `inline` keyword here is an ordinary code-size hint, not a correctness requirement.
NO_INSTRUMENT inline
FILE* get_thread_fp() {
    TraceGlobals& g = g_trace();
    if (!g.trace_ready.load(std::memory_order_acquire)) {
        return nullptr;
    }
    if (g.shutdown_complete.load(std::memory_order_relaxed)) {
        return nullptr;
    }
    if (!t_state.trace_file.open_attempted) {
        open_this_thread_file(t_state);
    }
    return t_state.trace_file.fp.load(std::memory_order_relaxed);
}

} // namespace

// Flushes per-thread stdio buffers and signals shutdown. Called via atexit() so it
// runs BEFORE static destructors (like the bfds() map). Idempotent via g.shutdown_started CAS.
//
// Shutdown race handling. A worker thread may be mid-fprintf (or mid-pwrite) when
// this runs. To bound the degraded mode to what the design promises — "EBADF at
// worst, never a crash" — we deliberately do NOT close other threads' descriptors
// here:
//
//   * fclose() would free the stdio FILE struct. POSIX says using a stream from
//     another thread after fclose() is UB; in practice a racing worker's fprintf
//     would dereference freed memory (worse than torn line — potential SIGSEGV).
//     So we only fflush(). Line-buffered mode has already pushed every completed
//     line through to the kernel; this catches any final partial line in stdio's
//     buffer. fflush on a FILE is thread-safe (each FILE has an implicit lock),
//     unlike fclose.
//
//   * close(patch_fd) would release the fd number, and fd numbers are reused
//     aggressively on Linux. A stale pwrite from a racing worker could then land
//     in whatever unrelated fd opens next — far worse than EBADF. Leaving both
//     fds open costs us one fd pair per thread until process exit, which the
//     kernel reclaims automatically.
//
// Per-thread ~PerThreadTraceFile still closes both descriptors normally on thread
// exit — that runs on the owning thread only, so no cross-thread race exists there.
// Workers that survive to process exit simply have their fds closed by the kernel.
//
// The open_files registry IS cleared here so per-thread destructors running during
// static cleanup don't try to find themselves in a partially-consistent vector;
// that registry pointer is the one thing shutdown uses to reach cross-thread state,
// and clearing it is racy-safe (workers don't touch it on the hot path — only
// open_this_thread_file and ~PerThreadTraceFile do, and both hold the mutex).
NO_INSTRUMENT
static void trace_shutdown() {
    t_state.in_instrumentation = true;  // Permanently disable on main — never cleared

    TraceGlobals& g = g_trace();

    // Idempotent: if another call already started shutdown, do nothing.
    bool expected = false;
    if (!g.shutdown_started.compare_exchange_strong(expected, true)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g.open_files_mutex);
        for (PerThreadTraceFile* f : g.open_files) {
            if (f == nullptr) continue;
            FILE* fp = f->fp.load(std::memory_order_relaxed);
            if (fp != nullptr) {
                fflush(fp);
            }
            // patch_fd intentionally not touched — see function comment.
        }
        g.open_files.clear();

        // Set the flag while still holding the mutex: open_this_thread_file()
        // checks shutdown_complete under the same lock before registering, so a
        // store outside this scope would leave a window where a late-opening
        // thread sees the flag unset and pushes into the just-cleared registry
        // (its file would then never be flushed by shutdown). Inside the lock,
        // "registry cleared" and "shutdown complete" become one atomic step.
        g.shutdown_complete.store(true, std::memory_order_release);
    }
}

// PerThreadTraceFile destructor: closes this thread's file when the thread exits.
// Only the owning thread ever runs this, so the close is race-free by construction
// — no other thread can still be dereferencing this thread's FILE* or patch_fd.
//
// trace_shutdown() no longer touches other threads' descriptors (see the comment
// on that function for why), so the atomic transitions here are effectively
// uncontended. They remain atomic only for consistency with the hot-path reads
// in __cyg_profile_func_enter/exit (where the reader is always the owning thread,
// but formally declaring the data atomic avoids any C++ memory-model hair-splitting
// if the design ever evolves).
//
// The mutex is held only while touching the shared open_files registry, which is
// small enough that the per-thread-exit cost is negligible.
NO_INSTRUMENT
PerThreadTraceFile::~PerThreadTraceFile() {
    // Set the re-entrancy guard permanently — the thread is about to die, and any
    // instrumented callee inside fclose/erase (possible with Clang) must be a no-op.
    t_state.in_instrumentation = true;

    // Fast path: never opened → nothing to close, nothing registered. Cheap
    // short-circuit for threads that never produced any trace output.
    if (fp.load(std::memory_order_relaxed) == nullptr) {
        return;
    }

    TraceGlobals& g = g_trace();
    std::lock_guard<std::mutex> lock(g.open_files_mutex);

    FILE* old = fp.exchange(nullptr, std::memory_order_relaxed);
#ifdef LOG_ELAPSED
    int old_pfd = patch_fd.exchange(-1, std::memory_order_relaxed);
#endif
    if (old == nullptr) {
        return;
    }

    fclose(old);
#ifdef LOG_ELAPSED
    if (old_pfd != -1) {
        close(old_pfd);
    }
#endif

    if (registered) {
        // Erase self from the registry. We hold a pointer to this, but after this
        // destructor the storage is gone, so leaving a dangling pointer would be bad.
        // Note that after trace_shutdown() has run, the registry is already empty;
        // find() returns end() and the erase is a no-op. Harmless either way.
        auto it = std::find(g.open_files.begin(), g.open_files.end(), this);
        if (it != g.open_files.end()) {
            g.open_files.erase(it);
        }
        registered = false;
    }
}

__attribute__ ((constructor))
NO_INSTRUMENT
void trace_begin() {
    t_state.in_instrumentation = true;

#ifdef LOG_ELAPSED
    // Belt-and-suspenders: the LOG_ELAPSED byte-offset derivation assumes
    // pretty_time() returns exactly PRETTY_TIME_LENGTH characters. If someone
    // changes LOGGER_PRETTY_TIME_FORMAT / LOGGER_PRETTY_MS_FORMAT without also
    // updating PRETTY_TIME_LENGTH, the unit test catches it — this runtime
    // check is a second line of defense for downstream consumers who might
    // somehow skip the unit test but still end up here. A manual check rather
    // than assert(), deliberately: README recommends RelWithDebInfo builds,
    // which define NDEBUG and would compile an assert away — this guard must
    // hold in every build type, aborting loudly rather than letting mis-offset
    // pwrites silently corrupt trace files.
    if (utils::pretty_time().size() != utils::PRETTY_TIME_LENGTH) {
        fprintf(stderr,
                "[call-stack-logger] FATAL: pretty_time() length differs from "
                "PRETTY_TIME_LENGTH — update include/prettyTime.h (LOG_ELAPSED "
                "byte offsets would corrupt the trace file)\n");
        abort();
    }
#endif

    // Capture the main thread's TID before any worker thread exists. Worker threads
    // compare their gettid() to this value to decide main-file vs per-tid-file.
    TraceGlobals& g = g_trace();
    g.main_tid = current_tid();

    // Resolve the base path from the env var (pure helper in traceFilePath.h).
    g.base_path = utils::resolve_base_trace_path(std::getenv("CSLG_OUTPUT_FILE"));

    // Eagerly open the main thread's file so the "=== New trace run ===" header
    // lands before main() starts. Preserves today's behavior for single-threaded
    // programs (main's file exists immediately after trace_begin returns).
    open_this_thread_file(t_state);

    // Register shutdown via atexit so it runs before static destructors.
    // Critical for Clang where static destructors (like the bfds() map) may be
    // instrumented — shutdown must close trace file(s) before those destructors fire.
    std::atexit(trace_shutdown);

    g.trace_ready.store(true, std::memory_order_release);
    t_state.in_instrumentation = false;
}

// Note: there used to be a trace_end() __attribute__((destructor)) here as a
// "fallback for _exit/abort". That comment was incorrect — _exit and abort do
// NOT run static destructors either, so trace_end never actually fired in the
// scenarios it claimed to cover. It only ever ran alongside the atexit-registered
// trace_shutdown (which already does the work), and accessed g_trace() at
// static-destruction time where the singleton may already be destroyed. Removed
// to eliminate the dead code and the theoretical UAF. Line-buffered output plus
// the kernel's close-on-exit guarantees cover the cases that trace_end did not.

extern "C" NO_INSTRUMENT
void __cyg_profile_func_enter(void *callee, void *caller) {
    if (t_state.in_instrumentation) { return; }
    // Set the guard BEFORE any work and clear it AFTER all local variables (especially
    // maybe_resolved, a std::optional<ResolvedFrame>) have been destroyed. With Clang,
    // destructors of std library types may be instrumented, so in_instrumentation must
    // remain true until all destructors have run.
    t_state.in_instrumentation = true;
    {
        // True when this frame produced a trace line: the file is open AND resolution
        // succeeded. Only logged frames adjust current_stack_depth on enter/exit and
        // (with LOG_ELAPSED) get a duration patch on exit.
        bool logged = false;
#ifdef LOG_ELAPSED
        off_t line_start = 0;
#endif
        // Exception barrier: a tracing hook must never inject an exception into the
        // traced program. Everything that can realistically throw (bad_alloc from
        // the std::string work in get_thread_fp's lazy open, resolve(), and
        // utils::format()) runs inside this try. State mutations (depth increment,
        // `logged`, cursor bookkeeping) happen only AFTER the last throwing
        // operation, so an exception leaves the bookkeeping untouched: the frame
        // simply goes untraced and the unconditional push below keeps enter/exit
        // pairing intact. The catch body must not allocate.
        try {
            FILE* fp = get_thread_fp();
            if (fp != nullptr) {
                auto maybe_resolved = instrumentation::resolve(callee, caller);
                if (maybe_resolved.has_value()) {
#ifdef LOG_ELAPSED
                    // Splice a fixed-width "[  pending ] " placeholder right after the
                    // timestamp. See include/durationFormat.h for the width invariant.
                    // format() may allocate (throw) — called with the prospective
                    // depth; the actual increment follows in the no-throw zone.
                    std::string line = utils::format(*maybe_resolved,
                                                     t_state.current_stack_depth + 1,
                                                     "[  pending ] ");
                    // Terminate while still in the throwing zone — push_back may
                    // reallocate.
                    line.push_back('\n');
                    // ---- no-throw zone ----
                    t_state.current_stack_depth++;
                    logged = true;
                    // Remember where this line starts: the placeholder offset for the
                    // per-frame slot is recorded after the push below.
                    line_start = t_state.cursor;
                    // No mutex: this FILE* is private to this thread. One fwrite of the
                    // newline-terminated line — a single stdio call (one FILE-lock
                    // round trip) with a known-exact byte count, so cursor tracking
                    // stays accurate without any ftello/fflush on the hot path. The
                    // trailing '\n' is the line-buffered flush point, so a failing
                    // write(2) (ENOSPC, EIO) surfaces as a short count here. On
                    // failure the cursor is not advanced and is marked invalid — an
                    // unknown number of bytes reached the file, so any further
                    // cursor-derived patch offset would corrupt existing lines.
                    if (fwrite(line.data(), 1, line.size(), fp) == line.size()) {
                        t_state.cursor += static_cast<off_t>(line.size());
                    } else {
                        t_state.cursor_valid = false;
                    }
#else
                    // format() may allocate (throw) — called with the prospective
                    // depth; the actual increment follows in the no-throw zone.
                    std::string line = utils::format(*maybe_resolved,
                                                     t_state.current_stack_depth + 1);
                    // Terminate while still in the throwing zone — push_back may
                    // reallocate.
                    line.push_back('\n');
                    // ---- no-throw zone ----
                    t_state.current_stack_depth++;
                    logged = true;
                    // No mutex: this FILE* is private to this thread. One fwrite of
                    // the newline-terminated line — a single stdio call with no
                    // per-line format-string parsing, mirroring the LOG_ELAPSED
                    // branch minus its cursor bookkeeping.
                    (void)fwrite(line.data(), 1, line.size(), fp);
#endif
                }
            } // maybe_resolved destructor runs here, still under guard
        } catch (...) {
            // Swallow (realistically only bad_alloc under OOM). The frame goes
            // untraced; `logged` stayed false and no state was half-updated.
        }

        // Push a frame record for EVERY call — even when nothing was logged (trace not
        // ready, open failed, resolution filtered the frame, or shutdown completed).
        // Pairing with __cyg_profile_func_exit must depend only on call structure: the
        // exit hook pops unconditionally, so gating this push on fp availability would
        // desynchronize the stack whenever availability changes between a frame's
        // enter and its exit. Concretely, enters that skipped pushing after shutdown
        // (or before trace_ready, on a thread spawned by an instrumented static
        // constructor) shifted every subsequent pop onto an ancestor's slot — and,
        // with LOG_ELAPSED, patched the wrong line's duration field.
        if (t_state.frame_resolved_top < MAX_TRACE_DEPTH - 1) {
            t_state.frame_resolved_stack[++t_state.frame_resolved_top] = logged;
#ifdef LOG_ELAPSED
            if (logged) {
                // Placeholder sits at "[<timestamp>] " offset = 1 + PRETTY_TIME_LENGTH + 2.
                // Only when the cursor is still trustworthy (this line's write
                // included — a failure in it invalidated the cursor above); with
                // an untrusted cursor, record the -1 sentinel so the exit handler
                // skips the pwrite and the placeholder stays "[  pending ]".
                static constexpr std::size_t PLACEHOLDER_OFFSET_IN_LINE =
                        utils::PRETTY_TIME_LENGTH + 3;
                t_state.frame_placeholder_offset[t_state.frame_resolved_top] =
                        t_state.cursor_valid
                        ? line_start + static_cast<off_t>(PLACEHOLDER_OFFSET_IN_LINE)
                        : static_cast<off_t>(-1);
                // Read the clock as the hook's LAST step for this frame, mirrored
                // by the exit hook reading it FIRST (before its own format +
                // pwrite work): the frame's reported span covers the function
                // body but not the tracer's own per-call work (resolve + format +
                // line write — microseconds even when the memoization caches hit,
                // tens of microseconds cold). Hook overhead of calls nested
                // INSIDE the function still lands in the parent's span —
                // unavoidable without per-frame overhead accounting.
                t_state.frame_enter_time[t_state.frame_resolved_top] =
                        std::chrono::steady_clock::now();
            }
#endif
        } else {
            // Stack full — track overflow to keep the exit handler in sync. Overflow
            // frames get no per-frame slot: recording one would overwrite the topmost
            // pushed frame's slot and corrupt its patch. With LOG_ELAPSED their
            // "[  pending ]" placeholder therefore stays un-patched even on clean
            // exit (accepted MAX_TRACE_DEPTH artifact; indentation stays correct).
            t_state.frame_overflow_count++;
        }
    }
    t_state.in_instrumentation = false;
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_exit(void *callee, void *caller) {
    // The exit hook needs neither address: pairing is positional (LIFO pop of the
    // per-thread frame stack). The parameters exist because the compiler-emitted
    // call passes them; silence -Wextra's unused-parameter warning.
    (void)callee;
    (void)caller;
    if (t_state.in_instrumentation) { return; }
#ifdef LOG_ELAPSED
    // Set the re-entrancy guard because below we call std::chrono::steady_clock::now()
    // and pwrite() — calls that are safe in trace.cpp (compiled without instrumentation)
    // but want protection from any exotic indirect instrumentation path. Without
    // LOG_ELAPSED the exit handler is mutex-free and I/O-free, so we keep the
    // zero-overhead guarantee by skipping these stores entirely.
    t_state.in_instrumentation = true;
#endif
    // Pop a frame record for EVERY call — the exact mirror of the unconditional push
    // in __cyg_profile_func_enter (see the comment there). Deliberately NO fp check:
    // everything touched here is thread_local, and gating pops on fp availability is
    // precisely what desynchronized the pairing across the shutdown boundary (enters
    // stopped pushing once shutdown_complete was set, while exits kept popping
    // against the still-open raw FILE*, shifting pops onto ancestors' slots).
    if (t_state.frame_overflow_count > 0) {
        // This exit corresponds to a frame that overflowed the stack.
        // Assume it was logged (the common case at extreme depth) and
        // decrement depth to keep indentation consistent. Clamp at -1 (empty)
        // so threads whose overflow frames never logged anything (e.g. the
        // file never opened) cannot drift the depth negative.
        t_state.frame_overflow_count--;
        if (t_state.current_stack_depth > -1) {
            t_state.current_stack_depth--;
        }
    } else if (t_state.frame_resolved_top >= 0) {
        // Save the index BEFORE decrement so LOG_ELAPSED can index into the
        // per-frame time/offset arrays at the same slot the enter handler
        // wrote to.
        const int idx = t_state.frame_resolved_top;
        const bool was_logged = t_state.frame_resolved_stack[idx];
        t_state.frame_resolved_top--;
        if (was_logged) {
            t_state.current_stack_depth--;
#ifdef LOG_ELAPSED
            // Compute elapsed and patch the matching line's placeholder.
            const auto exit_time = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    exit_time - t_state.frame_enter_time[idx]).count();
            // steady_clock is monotonic so elapsed shouldn't go negative,
            // but clamp defensively so the uint64_t cast stays safe across
            // any future clock-source quirks.
            const std::uint64_t ns = (elapsed < 0) ? 0 :
                    static_cast<std::uint64_t>(elapsed);
            char buf[utils::DURATION_FIELD_WIDTH + 1];
            utils::format_duration_12chars(ns, buf);
            const int pfd = t_state.trace_file.patch_fd.load(std::memory_order_relaxed);
            if (pfd >= 0 && t_state.frame_placeholder_offset[idx] >= 0) {
                // pwrite is atomic for our 12 bytes (well under PIPE_BUF) and
                // honors the explicit offset because patch_fd was opened WITHOUT
                // O_APPEND. Return value intentionally unchecked: the only
                // failure modes are racing shutdown closing pfd (EBADF, no
                // recovery possible) or a disk-I/O hardware error — in either
                // case the placeholder stays visible in the trace, which is
                // the documented degraded-but-readable mode.
                (void)pwrite(pfd, buf, utils::DURATION_FIELD_WIDTH,
                             t_state.frame_placeholder_offset[idx]);
            }
#endif
        }
    }
#ifdef LOG_ELAPSED
    t_state.in_instrumentation = false;
#endif
}

#else

// Hooks are compiled out: there is no re-entrancy to guard against. No-op
// definitions keep callStack.cpp's public API entry points linking in
// DISABLE_INSTRUMENTATION builds.
namespace instrumentation {
NO_INSTRUMENT bool enter_no_instrument_scope() { return false; }
NO_INSTRUMENT void exit_no_instrument_scope(bool) {}
} // namespace instrumentation

#endif
// clang-format on
