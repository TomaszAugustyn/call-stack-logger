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

// clang-format off
#ifndef DISABLE_INSTRUMENTATION

// Maximum tracked call depth for the frame resolution stack. Each entry is 1 byte (bool).
// If call depth exceeds this limit, an overflow counter prevents stack desynchronization.
// This value can be increased if needed — the memory cost is MAX_TRACE_DEPTH bytes per thread.
static constexpr int MAX_TRACE_DEPTH = 2048;

// Per-thread RAII wrapper around the thread's FILE*. On thread exit, the destructor
// closes the file and removes this instance from the global registry. Coordinates
// with trace_shutdown() via g_trace().open_files_mutex to prevent double-close.
struct PerThreadTraceFile {
    FILE* fp = nullptr;
    bool open_attempted = false;   // avoid retry after open failure
    bool registered = false;       // set true once added to g_trace().open_files

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
// instrumented by Clang when added in step 4.
struct PerThreadState {
    bool in_instrumentation = false;
    int current_stack_depth = -1;
    bool frame_resolved_stack[MAX_TRACE_DEPTH] = {};
    int frame_resolved_top = -1;
    int frame_overflow_count = 0;
    // Cached gettid() result (0 means not yet resolved). Avoids a syscall per trace call.
    pid_t cached_tid = 0;
    PerThreadTraceFile trace_file;
};

static thread_local PerThreadState t_state;

// Process-wide globals bundled into one struct, wrapped in a Meyers-singleton accessor.
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
    static TraceGlobals instance;
    return instance;
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
    fprintf(fp, "\n%s\n%s\n%s\n", frame.c_str(), middle.c_str(), frame.c_str());
    fflush(fp);
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

    // Same security-hardening flags as the original trace_begin(): O_NOFOLLOW to
    // prevent symlink attacks, 0600 permissions so traces (which can leak internal
    // paths and function names) are not world-readable.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
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

    // Line-buffered mode: flushes after each '\n' (every trace line). Crash-safe
    // without per-call fflush overhead.
    setvbuf(fp, NULL, _IOLBF, 0);
    self.trace_file.fp = fp;
    write_run_separator_header(fp, tid);

    // Register this file so trace_shutdown() can close it at program exit if the
    // thread is still alive. If shutdown has already completed (edge case: a new
    // thread starts tracing after atexit fired), close immediately instead of
    // leaking the fd.
    {
        std::lock_guard<std::mutex> lock(g.open_files_mutex);
        if (g.shutdown_complete.load(std::memory_order_relaxed)) {
            fclose(fp);
            self.trace_file.fp = nullptr;
            return;
        }
        g.open_files.push_back(&self.trace_file);
        self.trace_file.registered = true;
    }
}

// Returns the FILE* for the current thread, opening it on first use. Returns nullptr
// if trace is not ready, shutdown has completed, or the open failed earlier.
NO_INSTRUMENT
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
    return t_state.trace_file.fp;
}

} // namespace

// Closes all per-thread trace files and permanently disables instrumentation.
// Called via atexit() so it runs BEFORE static destructors (like s_bfds). Idempotent
// via g.shutdown_started CAS.
//
// Shutdown race: a worker may be mid-fprintf when this runs. We accept the race —
// worst case is a torn final line or a silent write to a just-closed fd (EBADF, no
// crash). Line-buffered mode bounds corruption to at most one line per thread, and
// workers observe shutdown_complete inside get_thread_fp() within one cache coherence
// delay of the store below.
NO_INSTRUMENT
static void trace_shutdown() {
    t_state.in_instrumentation = true;  // Permanently disable on main — never cleared

    TraceGlobals& g = g_trace();

    // Idempotent: if another call already started shutdown, do nothing.
    bool expected = false;
    if (!g.shutdown_started.compare_exchange_strong(expected, true)) {
        return;
    }

    // Drain the registry: close every file still open. Null the fp so late TLS
    // destructors (for threads that exit after this point) see null and skip.
    {
        std::lock_guard<std::mutex> lock(g.open_files_mutex);
        for (PerThreadTraceFile* f : g.open_files) {
            if (f != nullptr && f->fp != nullptr) {
                fclose(f->fp);
                f->fp = nullptr;
            }
        }
        g.open_files.clear();
    }

    g.shutdown_complete.store(true, std::memory_order_release);
}

// PerThreadTraceFile destructor: closes this thread's file when the thread exits.
// Coordinates with trace_shutdown() via open_files_mutex and the fp null-check:
//   - If the thread exits BEFORE shutdown: this destructor closes fp, removes self
//     from registry.
//   - If the thread exits AFTER shutdown: trace_shutdown() already closed fp and
//     nulled it, and cleared the registry. This destructor sees fp == nullptr and
//     does nothing — no double-close.
NO_INSTRUMENT
PerThreadTraceFile::~PerThreadTraceFile() {
    // Set the re-entrancy guard permanently — the thread is about to die, and any
    // instrumented callee inside fclose/erase (possible with Clang) must be a no-op.
    t_state.in_instrumentation = true;

    if (fp == nullptr) {
        return;  // fast path — either never opened or already closed by shutdown
    }

    TraceGlobals& g = g_trace();
    std::lock_guard<std::mutex> lock(g.open_files_mutex);

    // Re-check under lock: trace_shutdown() may have raced and already closed us.
    if (fp == nullptr) {
        return;
    }

    fclose(fp);
    fp = nullptr;

    if (registered) {
        // Erase self from the registry. We hold a pointer to this, but after this
        // destructor the storage is gone, so leaving a dangling pointer would be bad.
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
    // Critical for Clang where static destructors (like s_bfds map) may be
    // instrumented — shutdown must close trace file(s) before those destructors fire.
    std::atexit(trace_shutdown);

    g.trace_ready.store(true, std::memory_order_release);
    t_state.in_instrumentation = false;
}

__attribute__ ((destructor))
NO_INSTRUMENT
void trace_end() {
    // Fallback cleanup in case atexit didn't run (e.g., _exit() or abort()).
    trace_shutdown();
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_enter(void *callee, void *caller) {
    if (t_state.in_instrumentation) { return; }
    // Set the guard BEFORE any work and clear it AFTER all local variables (especially
    // maybe_resolved, a std::optional<ResolvedFrame>) have been destroyed. With Clang,
    // destructors of std library types may be instrumented, so in_instrumentation must
    // remain true until all destructors have run.
    t_state.in_instrumentation = true;
    FILE* fp = get_thread_fp();
    if (fp != nullptr) {
        auto maybe_resolved = instrumentation::resolve(callee, caller);
        bool resolved = maybe_resolved.has_value();
        if (t_state.frame_resolved_top < MAX_TRACE_DEPTH - 1) {
            t_state.frame_resolved_stack[++t_state.frame_resolved_top] = resolved;
        } else {
            // Stack full — track overflow to keep exit handler in sync.
            // Indentation (current_stack_depth) remains correct regardless.
            t_state.frame_overflow_count++;
        }
        if (resolved) {
            t_state.current_stack_depth++;
            // No mutex: this FILE* is private to this thread.
            fprintf(fp, "%s\n",
                    utils::format(*maybe_resolved, t_state.current_stack_depth).c_str());
        }
    } // maybe_resolved destructor runs here, still under guard
    t_state.in_instrumentation = false;
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_exit(void *callee, void *caller) {
    if (t_state.in_instrumentation) { return; }
    // Read fp once — don't call get_thread_fp() here since we never want lazy open
    // from an exit handler (there was no matching enter).
    if (t_state.trace_file.fp != nullptr) {
        if (t_state.frame_overflow_count > 0) {
            // This exit corresponds to a frame that overflowed the stack.
            // Assume it was resolved (the common case at extreme depth) and
            // decrement depth to keep indentation consistent.
            t_state.frame_overflow_count--;
            t_state.current_stack_depth--;
        } else if (t_state.frame_resolved_top >= 0) {
            bool was_resolved = t_state.frame_resolved_stack[t_state.frame_resolved_top--];
            if (was_resolved) {
                t_state.current_stack_depth--;
            }
        }
    }
}

#endif
// clang-format on
