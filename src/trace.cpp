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
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
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

#ifdef LOG_EXCEPTIONS
    #include "eventFormat.h"
    #include "exceptionEvents.h"
    #include "frameReconcile.h"
    #include <cstring>
    #include <exception>
    #include <functional>
    #include <unordered_map>
    #include <unwind.h>
#endif

// In-place line patching — the second, non-O_APPEND descriptor, the per-thread
// byte cursor and the fixed line layout it relies on — serves two options:
// LOG_ELAPSED rewrites the duration placeholder on exit, LOG_EXCEPTIONS marks the
// lines of frames that did not return normally. Either one switches it on.
#if defined(LOG_ELAPSED) || defined(LOG_EXCEPTIONS)
    #define CSLG_LINE_PATCHING 1
#endif

// The fixed-width duration placeholder spliced after the timestamp of every line
// (LOG_ELAPSED; see include/durationFormat.h for the width invariant). A macro so
// the default build passes the very same literal it always did.
#ifdef LOG_ELAPSED
    #define CSLG_DURATION_SPLICE "[  pending ] "
#else
    #define CSLG_DURATION_SPLICE ""
#endif

// clang-format off
#ifndef DISABLE_INSTRUMENTATION

// Initial capacity (in frames) of the per-thread frame stack. NOT a depth limit: the
// stack is a std::vector that doubles whenever it fills up, so depth accounting stays
// exact at any call depth and the hot path is allocation-free once the current
// capacity covers the program's deepest call chain. A FrameRecord is 1 byte in the
// default build and up to 56 bytes with every option on (the frame's site identity
// and level, the enter timestamp, the line offset and depth), so the first
// reservation costs a tracing thread between 2 KB and 112 KB of heap. Growth can
// only fail under OOM — see frame_overflow_count for how that fallback keeps
// enter/exit pairing exact.
static constexpr std::size_t INITIAL_FRAME_CAPACITY = 2048;

// One record per instrumented frame currently on a thread's stack, pushed by every
// enter hook and popped by every exit hook. The type is private to this TU on
// purpose: every std::vector<FrameRecord> member the hooks call is therefore
// instantiated here (compiled without instrumentation) — under Clang no user TU can
// supply an instrumented COMDAT copy that would fire a hook from inside the hooks.
struct FrameRecord {
    // True when the enter hook wrote a trace line for this frame (file open AND
    // resolution succeeded). Only logged frames adjust current_stack_depth on
    // enter/exit and (with LOG_ELAPSED) get a duration patch on exit.
    bool logged;
#ifdef LOG_EXCEPTIONS
    // The frame's identity for the reconciliation rules in frameReconcile.h: the
    // hook's two arguments and the frame's level (its canonical frame address,
    // see frame_level()). They let the exit hook find THIS frame's record instead
    // of blindly popping the top, and let any hook recognize records of frames
    // that left without an exit hook.
    const void* callee;
    const void* caller;
    const void* level;
    // Base name (function_base_name) of the frame's demangled name — a pointer
    // into the resolver's leaked name cache, null for an unlogged frame.
    // Compared with the base names DWARF gives for inlined functions when
    // records share a physical frame (see reclaim_dead_inlined_records_*).
    const std::string* name_base;
    // std::uncaught_exceptions() at enter. GCC runs the exit hook of a frame the
    // unwinder passes through while the exception is still in flight, so a
    // higher count at exit means the frame ended by exception; a destructor (or
    // its callees) running DURING unwinding sees the same count at both ends and
    // is not mistaken for one.
    int uncaught_at_enter;
#endif
#ifdef LOG_ELAPSED
    // Enter timestamp, used to compute the elapsed duration on exit.
    std::chrono::steady_clock::time_point enter_time;
#endif
#ifdef CSLG_LINE_PATCHING
    // Byte offset (into this thread's trace file) of the first byte of this
    // frame's line; -1 when the cursor was untrustworthy at enter time (then no
    // patch is ever written for the frame). Every patch offset — the duration
    // field's (LOG_ELAPSED) and the tree glyph's (LOG_EXCEPTIONS) — derives from
    // it and the fixed line layout (see LINE_PREFIX_EXTRA and
    // utils::tree_glyph_offset). off_t keeps its signedness explicit at the use site.
    off_t line_start;
    // Depth the line was written at, which places its tree glyph.
    int depth;
#endif
};

// RAII: blocks thread cancellation for the enclosing scope and restores the previous
// state on exit. Used by every tracer scope that contains a POSIX cancellation point
// (fwrite -> write, the lazy open, BFD's reads, pwrite, close). Two facts make this
// load-bearing rather than cosmetic:
//   * Both GCC and Clang emit the call to __cyg_profile_func_enter/exit as
//     non-throwing, so the instrumented function has no unwind entry for that call
//     site. ANY unwind that leaves a hook frame — a C++ exception or the forced
//     unwind glibc implements pthread_cancel with — terminates the process (verified:
//     "FATAL: exception not rethrown" from glibc, or std::terminate from the
//     personality routine, on both compilers).
//   * ~PerThreadTraceFile runs from __call_tls_dtors BEFORE glibc marks the thread
//     as exiting, so a still-pending request would be honored at its close() — a
//     forced unwind out of a destructor (implicitly noexcept) terminates too.
// With cancellation blocked here the tracer adds no cancellation points of its own:
// a pending request is acted on at the traced program's next cancellation point,
// exactly as if the program were not instrumented. Asynchronous cancellation
// (PTHREAD_CANCEL_ASYNCHRONOUS) stays unsupported, as it is for practically every
// library. pthread_setcancelstate is a few instructions on glibc — no syscall.
struct ScopedCancelDisable {
    int previous = PTHREAD_CANCEL_ENABLE;
    NO_INSTRUMENT ScopedCancelDisable() { pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous); }
    NO_INSTRUMENT ~ScopedCancelDisable() { pthread_setcancelstate(previous, nullptr); }
    ScopedCancelDisable(const ScopedCancelDisable&) = delete;
    ScopedCancelDisable& operator=(const ScopedCancelDisable&) = delete;
};

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

#ifdef CSLG_LINE_PATCHING
    // Second file descriptor to the same trace file, opened WITHOUT O_APPEND so
    // pwrite() can patch fixed-width fields of lines already written at explicit
    // byte offsets: the duration placeholder (LOG_ELAPSED) and the tree glyph and
    // duration flag of frames that ended abnormally (LOG_EXCEPTIONS). fp (above)
    // still uses O_APPEND for sequential line writes — the two fds share the
    // kernel inode; writes never overlap in byte range (fp writes NEW bytes
    // beyond EOF, patch_fd rewrites EXISTING bytes). Atomic for consistency with
    // fp: all accesses are owner-thread or mutex-ordered (trace_shutdown
    // deliberately never touches patch_fd), so the atomic is formal
    // belt-and-suspenders, not a contention point.
    std::atomic<int> patch_fd{-1};
#endif

    NO_INSTRUMENT PerThreadTraceFile() = default;
    NO_INSTRUMENT ~PerThreadTraceFile();   // defined after TraceGlobals

    PerThreadTraceFile(const PerThreadTraceFile&) = delete;
    PerThreadTraceFile& operator=(const PerThreadTraceFile&) = delete;
};

// Per-thread re-entrancy guard: true while this thread is inside the tracer (the
// hooks' own pipeline, the public API entry points, or the exit-time windows where
// tracing is permanently off). The hooks return immediately while it is set.
//
// Deliberately NOT a member of PerThreadState below. A plain `bool` thread_local has
// no destructor, so it is never "destroyed": its storage stays valid and readable
// for the whole life of the thread. That matters at process exit — on the main
// thread the C++ runtime destroys thread_local objects (t_state included) at the
// START of exit(), before atexit handlers run. trace_shutdown() and every hook
// fired from a static destructor or atexit handler afterwards still consult the
// guard; reading a member of the already-destroyed t_state there would be
// use-after-lifetime, formally undefined behavior. A standalone trivially-destructible
// guard has no lifetime end to worry about and no member-order dependency.
static thread_local bool t_in_instrumentation = false;

#ifdef LOG_EXCEPTIONS
// Key of the per-thread level cache (see frame_level()): a hook site, i.e. the
// return address of one call into a hook. A private key type keeps the
// std::unordered_map instantiation unique to this TU, for the same reason
// FrameRecord is private (a user TU cannot supply an instrumented copy of it).
struct HookSite {
    const void* address;
    NO_INSTRUMENT bool operator==(const HookSite& other) const { return address == other.address; }
};

struct HookSiteHash {
    NO_INSTRUMENT std::size_t operator()(const HookSite& site) const {
        return std::hash<const void*>{}(site.address);
    }
};
#endif

// All per-thread state bundled in one struct for readability.
struct PerThreadState {
    int current_stack_depth = -1;
    // The frame stack: one FrameRecord per instrumented frame currently on this
    // thread's call stack. Grown (reserve) only inside the enter hook's exception
    // barrier; see INITIAL_FRAME_CAPACITY for sizing.
    std::vector<FrameRecord> frames;
    // Number of frames the enter hook could NOT push because growing `frames`
    // threw bad_alloc. Such overflow frames are never logged (no line, no depth
    // change), so the exit hook only counts them down. Once non-zero, every
    // nested enter is counted here too — even if memory is available again — so
    // overflow frames always sit strictly above every pushed record and the exit
    // hook's "drain the counter, then pop" order pairs enters and exits exactly.
    int frame_overflow_count = 0;
    // Cached gettid() result (0 means not yet resolved). Avoids a syscall per trace call.
    pid_t cached_tid = 0;

#ifdef LOG_EXCEPTIONS
    // Extent of this thread's own stack (see StackBounds in frameReconcile.h),
    // resolved once by resolve_thread_stack_bounds() on the thread's first enter.
    instrumentation::StackBounds stack_bounds;
    bool stack_bounds_resolved = false;
    // Per hook site, the distance from the hook's own frame address to the level
    // (canonical frame address) of the frame that called the hook. Filled by
    // frame_level(); bounded by the number of hook call sites in the program.
    std::unordered_map<HookSite, std::ptrdiff_t, HookSiteHash> level_offsets;
    // Direct-mapped front cache of level_offsets, indexed by site address: one
    // compare on a hit, so the hot path never pays the hash map's machinery
    // (which, with the library built at -O0, costs more than everything else the
    // option adds per call). 256 slots x 16 bytes per thread.
    struct LevelCacheSlot {
        const void* site;
        std::ptrdiff_t distance;
    };
    static constexpr std::size_t LEVEL_CACHE_SLOTS = 256;
    LevelCacheSlot level_cache[LEVEL_CACHE_SLOTS] = {};
#endif

#ifdef CSLG_LINE_PATCHING
    // Running byte position for this thread's trace file. Seeded from the file's
    // end position when the file is opened (handles multi-run append where
    // earlier runs already wrote content + separator headers). Advanced by
    // exactly the number of bytes we wrote for every line — lets us compute
    // patch offsets without calling ftello() on the hot path.
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
    const bool prev = t_in_instrumentation;
    t_in_instrumentation = true;
    return prev;
}

NO_INSTRUMENT
void exit_no_instrument_scope(bool prev) {
    t_in_instrumentation = prev;
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

#ifdef LOG_ELAPSED
// Offset of the duration placeholder within a line: right after "[<timestamp>] ".
static constexpr std::size_t PLACEHOLDER_OFFSET_IN_LINE = utils::PRETTY_TIME_LENGTH + 3;
#endif

#ifdef CSLG_LINE_PATCHING
// Rewrites `count` bytes at `offset_in_line` of the line that starts at
// `line_start` through the patch descriptor. pwrite is atomic for these few
// bytes (well under PIPE_BUF) and honors the explicit offset because patch_fd
// was opened WITHOUT O_APPEND. A -1 line_start (untrusted cursor) or a missing
// patch descriptor leaves the line as the enter hook wrote it. Return value
// intentionally unchecked: the only failure modes are racing shutdown closing
// pfd (EBADF, no recovery possible) or a disk-I/O hardware error — in either
// case the line keeps what was written, the documented degraded-but-readable
// mode.
NO_INSTRUMENT
void patch_line_bytes(off_t line_start, std::size_t offset_in_line, const char* bytes,
                      std::size_t count) {
    const int pfd = t_state.trace_file.patch_fd.load(std::memory_order_relaxed);
    if (pfd < 0 || line_start < 0) {
        return;
    }
    (void)pwrite(pfd, bytes, count, line_start + static_cast<off_t>(offset_in_line));
}
#endif

#ifdef LOG_EXCEPTIONS
// Bytes between "[<timestamp>] " and the tree on every line of this build: the
// duration column (LOG_ELAPSED) and the address column (LOG_ADDR). Both are
// fixed-width, which is what makes the glyph offset derivable from a line's
// start and its depth alone.
static constexpr std::size_t LINE_PREFIX_EXTRA = 0
#ifdef LOG_ELAPSED
        + utils::DURATION_FIELD_WIDTH + 1
#endif
#ifdef LOG_ADDR
        + utils::ADDR_COLUMN_WIDTH
#endif
        ;

// Marks a logged frame's line as ended abnormally, `how` being
// utils::FRAME_END_EXCEPTION (an exception left the frame) or FRAME_END_JUMP (a
// non-local jump skipped its exit hook): the '|' of the line's "|_ " glyph is
// rewritten to that character, and with LOG_ELAPSED the duration field is
// rewritten to `field` — the flagged "[  unwound ]" for frames whose exit hook
// never ran, or nullptr when the exit hook has just written the measured,
// flagged duration itself. Lines at depth 0 have no glyph, so only the field
// can carry the mark there.
NO_INSTRUMENT
void mark_frame_end(const FrameRecord& record, char how, const char* field) {
    if (!record.logged || record.line_start < 0) {
        return;
    }
#ifdef LOG_ELAPSED
    if (field != nullptr) {
        patch_line_bytes(record.line_start, PLACEHOLDER_OFFSET_IN_LINE, field,
                         utils::DURATION_FIELD_WIDTH);
    }
#else
    (void)field;
#endif
    if (record.depth >= 1) {
        patch_line_bytes(record.line_start, utils::tree_glyph_offset(record.depth, LINE_PREFIX_EXTRA),
                         &how, 1);
    }
}

// State of the _Unwind_Backtrace walk in frame_level(): the hook site looked
// for and the CFA of the frame that resumes there.
struct LevelSearch {
    const void* site;
    std::uintptr_t cfa;
    bool matched; // the context resuming at `site` has been seen
    bool found;   // `cfa` is valid
};

// One step of the walk. The frame that resumes at `site` is recognized by its
// IP, but its CFA is read from the NEXT context: libgcc stores in a context the
// CFA of the frame it has just unwound, and LLVM's libunwind reports a frame's
// stack pointer, which for the caller of the matched frame is that frame's CFA
// too — the same value from both unwinders.
NO_INSTRUMENT
_Unwind_Reason_Code level_search_step(_Unwind_Context* context, void* argument) {
    LevelSearch* search = static_cast<LevelSearch*>(argument);
    if (search->matched) {
        search->cfa = _Unwind_GetCFA(context);
        search->found = true;
        return _URC_END_OF_STACK; // anything but _URC_NO_REASON stops the walk
    }
    if (reinterpret_cast<const void*>(_Unwind_GetIP(context)) == search->site) {
        search->matched = true;
    }
    return _URC_NO_REASON;
}

// The level of the frame that called a hook: its canonical frame address (CFA,
// the stack pointer just before the call that created the frame), derived from
// the hook's return address (`site`) and the hook's own frame address (`frame`).
//
// The distance between `frame` and that CFA is a constant of the hook site: the
// hook's frame address is the calling frame's stack pointer at the call minus
// the hook's fixed prologue, and the CFA lies a fixed frame size above that
// stack pointer. So the distance is measured ONCE per site — an unwinder walk
// that stops at the frame resuming at `site`, matched by address rather than by
// frame count so inlining of this helper can never shift it — and every later
// call is a hash lookup and an addition. The CFA rather than the hook's frame
// address is what the reconciliation rules need: two different functions called
// from the same place have the same CFA but frame addresses that differ by their
// frame sizes.
//
// A site whose frame the unwinder cannot describe (an object without unwind
// tables) gets the smallest distance any frame can have, which keeps its level
// at or below its true value and the ordering rules sound. Never throws: a
// failed cache insertion just repeats the measurement on the next call. Runs
// with no lock held (the walk consults the loader's tables).
NO_INSTRUMENT
const void* frame_level(const void* site, const void* frame) {
    // Sites are instruction addresses following a call, so the low bits vary:
    // drop the two lowest and take the next eight as the slot.
    PerThreadState::LevelCacheSlot& slot =
            t_state.level_cache[(reinterpret_cast<std::uintptr_t>(site) >> 2) % PerThreadState::LEVEL_CACHE_SLOTS];
    if (slot.site == site) {
        return static_cast<const char*>(frame) + slot.distance;
    }
    const HookSite key{ site };
    auto it = t_state.level_offsets.find(key);
    if (it != t_state.level_offsets.end()) {
        slot.site = site;
        slot.distance = it->second;
        return static_cast<const char*>(frame) + it->second;
    }
    // Smallest possible distance: the call into the frame leaves its return
    // address 8 bytes below the CFA, and the hook's prologue puts a return
    // address and a saved frame pointer (16 bytes) below the frame's stack
    // pointer — 24 bytes for a frame with no locals of its own.
    std::ptrdiff_t distance = 24;
    LevelSearch search{ site, 0, false, false };
    _Unwind_Backtrace(level_search_step, &search);
    const std::uintptr_t frame_address = reinterpret_cast<std::uintptr_t>(frame);
    if (search.found && search.cfa > frame_address) {
        distance = static_cast<std::ptrdiff_t>(search.cfa - frame_address);
    }
    try {
        t_state.level_offsets.emplace(key, distance);
    } catch (...) {
        // Out of memory: not in the map, measured again on the next front-cache miss.
    }
    slot.site = site;
    slot.distance = distance;
    return static_cast<const char*>(frame) + distance;
}

// Pops `count` records from the top of this thread's frame stack whose frames are
// gone without having run their exit hook (see frameReconcile.h for how they are
// recognized) and marks their lines with `how` (see mark_frame_end). A logged
// record's frame had raised the depth on enter, and the exit hook that would have
// lowered it never ran, so the depth comes back down here. Never allocates or
// throws: pop_back() keeps the vector's capacity.
NO_INSTRUMENT
void reclaim_dead_records(std::size_t count, char how) {
#ifdef LOG_ELAPSED
    // No exit hook ran for these frames, so no duration exists: the placeholder
    // becomes the flagged "[  unwound ]".
    char field[utils::DURATION_FIELD_WIDTH + 1];
    std::memcpy(field, utils::DURATION_UNWOUND, sizeof(field));
    utils::set_duration_flag(field, how);
#else
    const char* const field = nullptr;
#endif
    while (count-- > 0) {
        const FrameRecord dead = t_state.frames.back();
        t_state.frames.pop_back();
        if (dead.logged) {
            t_state.current_stack_depth--;
            mark_frame_end(dead, how, field);
        }
    }
}

// True when `record` is a logged frame whose base name equals `base` (see
// function_base_name in frameReconcile.h).
NO_INSTRUMENT
bool record_has_base_name(const FrameRecord& record, const std::string& base) {
    return record.name_base != nullptr && !record.name_base->empty() && *record.name_base == base;
}

// Reclaims dead inlined records sitting ABOVE the parent of a frame that is
// being entered by a normal call: `parent_base` is the base name of the
// innermost function containing the call site (cached with the site's
// location, so this costs no lookup), i.e. the frame's direct parent. Records
// at the parent's level above the parent's own record are frames inlined into
// that physical frame that were entered after the parent and never exited
// (an inlined leaf that longjmp'ed out of a retry loop, a frame inlined into a
// catcher and unwound before the next call). Nothing is known, and nothing
// touched, when the parent's record is not found in its group.
NO_INSTRUMENT
void reclaim_dead_inlined_records_above_parent(const instrumentation::FrameKey& key,
                                               const std::string* parent_base, char how) {
    if (parent_base == nullptr || parent_base->empty() || t_state.frames.empty()) {
        return;
    }
    const FrameRecord& top = t_state.frames.back();
    if (!(instrumentation::stack_address(top.level) > instrumentation::stack_address(key.level))
        || !instrumentation::on_thread_stack(t_state.stack_bounds, top.level)
        || !instrumentation::on_thread_stack(t_state.stack_bounds, key.level)) {
        return;
    }
    const void* parent_level = top.level;
    reclaim_dead_records(instrumentation::dead_records_above_match(
                                 t_state.frames.data(), t_state.frames.size(), parent_level,
                                 [parent_base](const FrameRecord& record) {
                                     return record_has_base_name(record, *parent_base);
                                 }),
                         how);
}

// Reclaims dead records that share the level of the frame ENTERING at `site`
// (the enter hook's return address, minus one) — frames inlined into the same
// physical frame that an exception unwound or a longjmp skipped, which the
// level rules alone cannot tell from live inline hosts. With `chain` the inline
// chain at that site (innermost first; empty when the site has no DWARF
// location, in which case nothing is touched):
//   * a top record with the entering frame's own callee, caller and level is a
//     dead frame that was re-called from the same site after leaving without an
//     exit hook (a setjmp/longjmp retry loop) — unless the chain shows the
//     entering function inlined into itself, a recursive inlined copy whose
//     outer activation that record is;
//   * then, if the chain names the entering frame's inline host, every record
//     of the group above the host's record is a dead inlined frame.
NO_INSTRUMENT
void reclaim_dead_inlined_records_on_enter(const instrumentation::FrameKey& key, const void* site, char how) {
    if (t_state.frames.empty() || t_state.frames.back().level != key.level
        || !instrumentation::on_thread_stack(t_state.stack_bounds, key.level)) {
        return;
    }
    const std::vector<std::string>* chain = instrumentation::inline_chain_at(const_cast<void*>(site));
    if (chain == nullptr || chain->empty()) {
        return;
    }
    const bool recursive_inline = chain->size() >= 2 && (*chain)[0] == (*chain)[1];
    while (!recursive_inline && !t_state.frames.empty()
           && instrumentation::same_activation_site(t_state.frames.back(), key)
           && t_state.frames.back().level == key.level) {
        reclaim_dead_records(1, how);
    }
    if (chain->size() >= 2) {
        const std::string& host = (*chain)[1];
        reclaim_dead_records(instrumentation::dead_records_above_match(
                                     t_state.frames.data(), t_state.frames.size(), key.level,
                                     [&host](const FrameRecord& record) {
                                         return record_has_base_name(record, host);
                                     }),
                             how);
    }
}

// Reclaims dead records that share the CATCHER's level: frames inlined into the
// catcher's physical frame that the exception unwound. `chain` is the inline
// chain at the catch site, whose innermost entry is the function that caught;
// every record of the group above that function's record is dead.
NO_INSTRUMENT
void reclaim_dead_inlined_records_on_catch(const void* catch_level, const void* site) {
    if (t_state.frames.empty() || t_state.frames.back().level != catch_level
        || !instrumentation::on_thread_stack(t_state.stack_bounds, catch_level)) {
        return;
    }
    const std::vector<std::string>* chain = instrumentation::inline_chain_at(const_cast<void*>(site));
    if (chain == nullptr || chain->empty()) {
        return;
    }
    const std::string& catcher = (*chain)[0];
    reclaim_dead_records(instrumentation::dead_records_above_match(
                                 t_state.frames.data(), t_state.frames.size(), catch_level,
                                 [&catcher](const FrameRecord& record) {
                                     return record_has_base_name(record, catcher);
                                 }),
                         utils::FRAME_END_EXCEPTION);
}

// Resolves the extent of this thread's own stack once, with pthread_getattr_np().
// For the main thread glibc reads /proc/self/maps to find the stack mapping, so
// this may allocate — it is only ever called from inside the enter hook's exception
// barrier. If the lookup fails the bounds stay unknown, which the reconciliation
// rules treat as "the thread has one stack" (the case for every program that does
// not switch stacks itself).
NO_INSTRUMENT
void resolve_thread_stack_bounds() {
    if (t_state.stack_bounds_resolved) {
        return;
    }
    t_state.stack_bounds_resolved = true;
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return;
    }
    void* lowest = nullptr;
    std::size_t size = 0;
    if (pthread_attr_getstack(&attr, &lowest, &size) == 0 && size > 0) {
        t_state.stack_bounds.lo = reinterpret_cast<std::uintptr_t>(lowest);
        t_state.stack_bounds.hi = t_state.stack_bounds.lo + size;
        t_state.stack_bounds.known = true;
    }
    pthread_attr_destroy(&attr);
}
#endif

// Writes the "=== New trace run: <timestamp>, thread ID: <tid> ===" header framed
// above and below by `=` lines of matching length. Called immediately after a
// thread's file is lazily opened on its first traced call.
NO_INSTRUMENT
void write_run_separator_header(FILE* fp, pid_t tid) {
    std::string middle = "=== New trace run: " + utils::pretty_time()
                       + ", thread ID: " + std::to_string(tid) + " ===";
    std::string frame(middle.size(), '=');
    // No fflush needed: setvbuf(_IOLBF) is already in effect, so each '\n' flushes.
    fprintf(fp, "\n%s\n%s\n%s\n", frame.c_str(), middle.c_str(), frame.c_str());
}

// Opens this thread's trace file (lazily, on the thread's first traced call —
// main thread included; see the note in trace_begin()).
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

#ifdef CSLG_LINE_PATCHING
    // Second fd to the same file, WITHOUT O_APPEND, reserved for pwrite()-patching
    // fixed-width fields of lines already written: the "[  pending ]" placeholder
    // with the real duration on exit (LOG_ELAPSED), the tree glyph and duration
    // flag of frames that ended abnormally (LOG_EXCEPTIONS). A single O_APPEND fd
    // would force every pwrite to EOF on Linux (documented in pwrite(2)), so we
    // need this separate handle. dup() can't provide it either:
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
        // durations or markers. Keep tracing through fp; with pfd == -1 and
        // cursor_valid false, every frame records the -1 line-start sentinel, no
        // patch is ever written, and each line keeps what the enter hook wrote
        // ("[  pending ]", plain "|_ ") — the same documented degraded mode as a
        // mid-run write failure. Realistic triggers: /proc not mounted (minimal
        // chroot/container), fd limit exhaustion.
        fprintf(stderr,
                "[call-stack-logger] WARNING: Could not reopen %s for in-place patching "
                "— tracing continues, but durations stay \"[  pending ]\" and no exit "
                "markers are written\n",
                path.c_str());
    }
#endif

    // Line-buffered mode: flushes after each '\n' (every trace line). Crash-safe
    // without per-call fflush overhead.
    setvbuf(fp, NULL, _IOLBF, 0);
    self.trace_file.fp.store(fp, std::memory_order_relaxed);
#ifdef CSLG_LINE_PATCHING
    self.trace_file.patch_fd.store(pfd, std::memory_order_relaxed);
#endif

    write_run_separator_header(fp, tid);

#ifdef CSLG_LINE_PATCHING
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
#ifdef CSLG_LINE_PATCHING
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

// Flushes per-thread stdio buffers and signals shutdown. Registered via atexit()
// in trace_begin(). Exit handlers run in REVERSE registration order, so anything
// registered later — including destructors of function-local statics first
// constructed during main() — runs BEFORE this handler. That ordering is exactly
// why the resolver's static caches in callStack.h are deliberately leaked: with
// real destructors they would be destroyed before shutdown_complete is set here,
// while worker threads can still be inside resolve(). Conversely, statics
// constructed BEFORE trace_begin ran (user globals, under normal link order)
// destruct AFTER this handler — their possibly-instrumented destructors then hit
// disabled hooks. Idempotent via g.shutdown_started CAS.
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
    t_in_instrumentation = true;  // Permanently disable on main — never cleared

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
    // The guard lives outside t_state (see its definition), so it stays valid after
    // this destructor returns — on the main thread that is the rest of exit().
    t_in_instrumentation = true;
    // close() below is a cancellation point and this destructor runs before glibc
    // marks the exiting thread as such — see ScopedCancelDisable.
    ScopedCancelDisable no_cancel;

    // Fast path: never opened → nothing to close, nothing registered. Cheap
    // short-circuit for threads that never produced any trace output.
    if (fp.load(std::memory_order_relaxed) == nullptr) {
        return;
    }

    TraceGlobals& g = g_trace();
    std::lock_guard<std::mutex> lock(g.open_files_mutex);

    FILE* old = fp.exchange(nullptr, std::memory_order_relaxed);
#ifdef CSLG_LINE_PATCHING
    int old_pfd = patch_fd.exchange(-1, std::memory_order_relaxed);
#endif
    if (old == nullptr) {
        return;
    }

    fclose(old);
#ifdef CSLG_LINE_PATCHING
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
    t_in_instrumentation = true;

#ifdef CSLG_LINE_PATCHING
    // Belt-and-suspenders: the in-place patch offsets (LOG_ELAPSED, LOG_EXCEPTIONS)
    // assume pretty_time() returns exactly PRETTY_TIME_LENGTH characters. If someone
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
                "PRETTY_TIME_LENGTH — update include/prettyTime.h (in-place patch "
                "offsets would corrupt the trace file)\n");
        abort();
    }
#endif

    // Capture the main thread's TID before any worker thread exists. Worker threads
    // compare their gettid() to this value to decide main-file vs per-tid-file.
    TraceGlobals& g = g_trace();
    g.main_tid = current_tid();

    // Resolve the base path from the env var (pure helper in traceFilePath.h),
    // then anchor a relative path to the startup working directory. Every
    // thread opens its file lazily on its first traced call, so without this a
    // chdir() between the main thread's open and a worker's would scatter one
    // run's per-thread files across directories. If getcwd() fails (directory
    // unlinked, or a path longer than PATH_MAX) the relative path is kept as is.
    g.base_path = utils::resolve_base_trace_path(std::getenv("CSLG_OUTPUT_FILE"));
    char cwd[PATH_MAX];
    const char* cwd_or_null = getcwd(cwd, sizeof(cwd));
    g.base_path = utils::make_absolute_trace_path(g.base_path, cwd_or_null);

    // The main thread's file is deliberately NOT opened here: get_thread_fp()
    // opens it lazily on the first traced call, exactly like worker threads.
    // This constructor also runs in programs that link the plain library only
    // for the on-demand get_call_stack() API (callStack.cpp references the
    // re-entrancy guard functions above, which pulls this TU in from the static
    // archive) — an eager open would create a stray trace file for programs
    // that never trace anything. Instrumented programs are unaffected: their
    // first hook fires no later than main()'s own enter.

    // Register shutdown via atexit. Handlers run in reverse registration order,
    // so registering this early (pre-main) means destructors of user statics
    // constructed before trace_begin run AFTER shutdown — their possibly
    // instrumented destructors (a real concern under Clang) find the hooks
    // already disabled instead of half-dead state. Resolver-side statics need
    // no ordering at all: they are deliberately leaked (see callStack.h).
    std::atexit(trace_shutdown);

#ifdef LOG_EXCEPTIONS
    // Before any throw can be traced: make sure the interposers can forward to
    // the C++ runtime (aborts with a clear message if the runtime is linked
    // statically) and are the active definitions (warns otherwise).
    instrumentation::events::verify_interposers();
#endif

    g.trace_ready.store(true, std::memory_order_release);
    t_in_instrumentation = false;
}

#ifdef LOG_EXCEPTIONS
namespace {

// Column word of an event line with LOG_ELAPSED (the 12-byte word plus the
// separating space, like the "[  pending ] " splice of a call line), or nothing.
#ifdef LOG_ELAPSED
constexpr const char* EVENT_COLUMN_THROW = "[  throw   ] ";
constexpr const char* EVENT_COLUMN_RETHROW = "[ rethrow  ] ";
constexpr const char* EVENT_COLUMN_CATCH = "[  catch   ] ";
constexpr const char* EVENT_COLUMN_TERMINATE = "[ terminate] ";
static_assert(sizeof("[  throw   ] ") - 1 == utils::DURATION_FIELD_WIDTH + 1, "column word + space");
static_assert(sizeof("[ rethrow  ] ") - 1 == utils::DURATION_FIELD_WIDTH + 1, "column word + space");
static_assert(sizeof("[  catch   ] ") - 1 == utils::DURATION_FIELD_WIDTH + 1, "column word + space");
static_assert(sizeof("[ terminate] ") - 1 == utils::DURATION_FIELD_WIDTH + 1, "column word + space");
#else
constexpr const char* EVENT_COLUMN_THROW = "";
constexpr const char* EVENT_COLUMN_RETHROW = "";
constexpr const char* EVENT_COLUMN_CATCH = "";
constexpr const char* EVENT_COLUMN_TERMINATE = "";
#endif

// Demangled exception type names, memoized per type_info (demangling allocates
// and costs microseconds; a program throws the same few types over and over).
// Leaked like the resolver's caches; its own small mutex since events are rare.
NO_INSTRUMENT
const std::string& exception_type_name(const std::type_info* type) {
    static const std::string unknown("<unknown type>");
    if (type == nullptr) {
        return unknown;
    }
    static std::mutex* const mutex = new std::mutex;
    static auto* const cache = new std::unordered_map<const std::type_info*, std::string>();
    std::lock_guard<std::mutex> lock(*mutex);
    auto it = cache->find(type);
    if (it == cache->end()) {
        it = cache->emplace(type, instrumentation::demangle_symbol(type->name())).first;
    }
    return it->second;
}

// The resolved location of an event's site: an address inside the throwing,
// catching or terminating instruction (the interposer's return address minus
// one), resolved like a call site through the same caches.
struct EventSite {
    const void* address = nullptr;
    instrumentation::ResolvedFrameView location;
};

NO_INSTRUMENT
EventSite resolve_event_site(const void* address) {
    EventSite site;
    site.address = address;
    if (address != nullptr) {
        instrumentation::resolve_site(const_cast<void*>(address), site.location);
    }
    return site;
}

// Writes one exception event line (see include/eventFormat.h) as a child of the
// innermost logged frame — at depth current_stack_depth + 1 — and advances the
// byte cursor exactly like a call line does. `site` is the event's resolved
// site, or one with a null address for an event without a site (a terminate
// line for an exception the runtime gave up on). Runs inside the calling
// function's guard, cancellation block and exception barrier.
NO_INSTRUMENT
void write_event_line(const char* verb, const char* site_label, const char* suffix, const char* column,
                      const char* type_name, const char* what, const EventSite& site) {
    FILE* fp = get_thread_fp();
    if (fp == nullptr) {
        return;
    }
    char timestamp[utils::PRETTY_TIME_BUF_SIZE];
    utils::pretty_time_into(timestamp, sizeof(timestamp));
    utils::EventLine event;
    event.timestamp = timestamp;
#ifdef LOG_ADDR
    if (site.address != nullptr) {
        event.site_address = const_cast<void*>(site.address);
    }
#endif
    event.depth = t_state.current_stack_depth + 1;
    event.verb = verb;
    event.type_name = type_name;
    event.what = what;
    event.site_label = site_label;
    event.site_file = site.location.caller_filename;
    event.site_line = site.location.caller_line_number;
    event.suffix = suffix;

    char line[utils::FORMAT_BUF_SIZE];
    const std::size_t line_size =
            utils::format_event_into(line, sizeof(line), event, column, /*append_newline=*/true);
    // Cursor bookkeeping as for a call line: a short write leaves an unknown
    // number of bytes in the file, so later patch offsets could not be trusted.
    if (fwrite(line, 1, line_size, fp) == line_size) {
        t_state.cursor += static_cast<off_t>(line_size);
    } else {
        t_state.cursor_valid = false;
    }
}

} // namespace

namespace instrumentation {
namespace events {

NO_INSTRUMENT
bool inside_tracer() {
    return t_in_instrumentation;
}

NO_INSTRUMENT
void on_throw(ThrowKind kind, const std::type_info* type, const char* what, const void* site) {
    if (t_in_instrumentation) {
        return;
    }
    // Same discipline as the hooks: cancellation blocked around the file write,
    // the guard set for the whole duration, and an exception barrier — an
    // exception escaping here would replace the program's exception with ours.
    ScopedCancelDisable no_cancel;
    t_in_instrumentation = true;
    try {
        const EventSite at = resolve_event_site(site);
        const char* type_name = exception_type_name(type).c_str();
        switch (kind) {
        case ThrowKind::primary:
            write_event_line("throw", "thrown at", "", EVENT_COLUMN_THROW, type_name, what, at);
            break;
        case ThrowKind::rethrow:
            write_event_line("rethrow", "rethrown at", "", EVENT_COLUMN_RETHROW, type_name, what, at);
            break;
        case ThrowKind::exception_ptr:
            write_event_line("rethrow", "rethrown at", " via std::rethrow_exception", EVENT_COLUMN_RETHROW,
                             type_name, what, at);
            break;
        }
    } catch (...) {
        // Swallow (realistically only bad_alloc under OOM): the event goes untraced.
    }
    t_in_instrumentation = false;
}

namespace {

// The what() of the exception being handled, read through public interfaces
// only: the exception is rethrown into a local handler (the interposers stay
// silent while the guard is set, so nothing is logged for it). Null when no
// exception is being handled or it is not a std::exception. Used for the
// terminate line of a std::terminate() call, where no object is at hand.
NO_INSTRUMENT
const char* current_exception_what(char (&buffer)[utils::WHAT_TEXT_CAPACITY]) {
    const std::exception_ptr current = std::current_exception();
    if (!current) {
        return nullptr;
    }
    try {
        std::rethrow_exception(current);
    } catch (const std::exception& e) {
        utils::sanitize_what_into(e.what(), buffer, sizeof(buffer));
        return buffer;
    } catch (...) {
    }
    return nullptr;
}

// The name of Clang's noexcept landing-pad stub: a hidden function in the
// executable (or shared library) that calls __cxa_begin_catch and then
// std::terminate(), where GCC 13+ calls the runtime's __cxa_call_terminate. The
// catch interposer's return address lies inside it, and the resolver names it
// from the symbol table.
constexpr const char* CLANG_TERMINATE_STUB = "__clang_call_terminate";

} // namespace

NO_INSTRUMENT
void on_terminate(TerminateReason reason, const std::type_info* type, const char* what, const void* site) {
    if (t_in_instrumentation) {
        return;
    }
    ScopedCancelDisable no_cancel;
    t_in_instrumentation = true;
    try {
        const EventSite at = resolve_event_site(site);
        const char* label = "no handler found";
        const char* type_name = exception_type_name(type).c_str();
        char buffer[utils::WHAT_TEXT_CAPACITY];
        switch (reason) {
        case TerminateReason::no_handler:
            break;
        case TerminateReason::noexcept_boundary:
            label = "thrown across a noexcept boundary";
            break;
        case TerminateReason::terminate_called:
            label = "std::terminate called at";
            if (type == nullptr) {
                // Nothing is being handled: either a plain std::terminate() call, or
                // an exception still in flight that the caller did not catch first
                // (a noexcept violation compiled by GCC before 13).
                type_name = std::uncaught_exceptions() > 0 ? "(exception in flight)" : "(no active exception)";
            }
            break;
        }
        if (type != nullptr && what == nullptr) {
            // A dependent exception, or a std::terminate() call: the interposer had
            // no object to read; the exception being handled is the one to name.
            what = current_exception_what(buffer);
        }
        write_event_line("terminate", label, "", EVENT_COLUMN_TERMINATE, type_name, what, at);
    } catch (...) {
        // Swallow (realistically only bad_alloc under OOM): the event goes untraced.
    }
    t_in_instrumentation = false;
}

NO_INSTRUMENT
bool on_catch(const std::type_info* type, const char* what, bool dependent, const void* wrapper_site,
              const void* wrapper_frame) {
    if (t_in_instrumentation) {
        return false;
    }
    bool terminating = false;
    ScopedCancelDisable no_cancel;
    t_in_instrumentation = true;
    try {
        const EventSite at = resolve_event_site(static_cast<const char*>(wrapper_site) - 1);
        const char* type_name = exception_type_name(type).c_str();
        char buffer[utils::WHAT_TEXT_CAPACITY];
        if (dependent && what == nullptr) {
            what = current_exception_what(buffer);
        }
        if (at.location.caller_function_base != nullptr
            && *at.location.caller_function_base == CLANG_TERMINATE_STUB) {
            // Not a catch: the exception hit a noexcept boundary and the program
            // is terminating (GCC's equivalent, __cxa_call_terminate, is recognized
            // by the interposer itself; GCC also ran the unwound frames' exit hooks
            // first). The stub is called from the noexcept function's landing pad,
            // so every record at or below the stub's level is a frame the exception
            // unwound — the stub now occupies that stack slot — and is reclaimed,
            // which puts the terminate line under the noexcept function.
            resolve_thread_stack_bounds();
            const char* stub_level = static_cast<const char*>(frame_level(wrapper_site, wrapper_frame));
            reclaim_dead_records(instrumentation::dead_records_on_catch(t_state.frames.data(),
                                                                        t_state.frames.size(), stub_level + 1,
                                                                        t_state.stack_bounds),
                                 utils::FRAME_END_EXCEPTION);
            write_event_line("terminate", "thrown across a noexcept boundary", "", EVENT_COLUMN_TERMINATE,
                             type_name, what, EventSite{});
            terminating = true;
        } else {
            // The interposer was called from the catcher's landing pad, so the level
            // derived from its return address and frame address is the catcher's
            // own. Every record below it belongs to a frame the exception unwound:
            // on Clang those frames ran no exit hook, so this is where they are
            // reclaimed and their lines marked; on GCC their exit hooks already
            // popped them and nothing is left below the catcher. Then the catch
            // line lands at the catcher's depth + 1.
            resolve_thread_stack_bounds();
            const void* catch_level = frame_level(wrapper_site, wrapper_frame);
            reclaim_dead_records(instrumentation::dead_records_on_catch(t_state.frames.data(),
                                                                        t_state.frames.size(), catch_level,
                                                                        t_state.stack_bounds),
                                 utils::FRAME_END_EXCEPTION);
            // Frames inlined into the catcher that the exception unwound share the
            // catcher's level; the DWARF inline chain at the catch site tells them
            // apart.
            reclaim_dead_inlined_records_on_catch(catch_level, at.address);
            write_event_line("catch", "caught at", "", EVENT_COLUMN_CATCH, type_name, what, at);
        }
    } catch (...) {
        // Swallow (realistically only bad_alloc under OOM): the event goes untraced.
    }
    t_in_instrumentation = false;
    return terminating;
}

} // namespace events
} // namespace instrumentation
#endif

// Note: there used to be a trace_end() __attribute__((destructor)) here as a
// "fallback for _exit/abort". That comment was incorrect — _exit and abort do
// NOT run static destructors either, so trace_end never actually fired in the
// scenarios it claimed to cover. It only ever ran alongside the atexit-registered
// trace_shutdown (which already does the work), and accessed g_trace() at
// static-destruction time where the singleton may already be destroyed. Removed
// to eliminate the dead code and the theoretical UAF. Line-buffered output plus
// the kernel's close-on-exit guarantees cover the cases that trace_end did not.

// `caller` is the instrumented function's own return address (both compilers
// pass __builtin_return_address(0)), i.e. it already points into the real
// caller — instrumentation::resolve() only steps it back into the call
// instruction. No stack walk happens anywhere in the hook.
extern "C" NO_INSTRUMENT
void __cyg_profile_func_enter(void *callee, void *caller) {
    if (t_in_instrumentation) { return; }
    // Block cancellation for the whole hook — see ScopedCancelDisable for why this
    // is required. The same fact (no unwind entry at the hook's call site) is why
    // the exception barrier below must never let anything escape.
    ScopedCancelDisable no_cancel;
    // Set the guard BEFORE any work and clear it only as the very last step. The
    // hot path below holds no std objects any more (stack buffers and a non-owning
    // view), but the cold path (a cache miss inside resolve) still runs std
    // container/string code, and with Clang the destructors of std library types
    // may be instrumented — the guard must stay set until every one of them has run.
    t_in_instrumentation = true;
    if (t_state.frame_overflow_count > 0) {
        // An enclosing frame overflowed (its record could not be pushed — see
        // frame_overflow_count), so this nested frame overflows too: no line, no
        // depth change, just a count for the exit hook to drain. This keeps every
        // overflow frame strictly above every pushed record.
        t_state.frame_overflow_count++;
    } else {
        // See FrameRecord::logged. Stays false unless a trace line was written.
        bool logged = false;
        // True once `frames` is guaranteed to have room for this frame's record.
        bool have_slot = false;
#ifdef CSLG_LINE_PATCHING
        off_t line_start = 0;
#endif
#ifdef LOG_EXCEPTIONS
        // This frame's identity for the reconciliation rules (frameReconcile.h). The
        // hook's return address and frame address must be read here, in the hook
        // itself; the level is derived from them inside the barrier below, since a
        // first sight of this hook site caches its distance (frame_level()).
        const void* const hook_site = __builtin_return_address(0);
        const void* const hook_frame = __builtin_frame_address(0);
        instrumentation::FrameKey key{ callee, caller, nullptr };
        const std::string* frame_base_name = nullptr;
#endif
        // Exception barrier: a tracing hook must never inject an exception into the
        // traced program. Everything that can realistically throw (bad_alloc from
        // growing the frame stack, from the std::string work in get_thread_fp's
        // lazy open, and from resolve() when an address is seen for the first time
        // and its result is inserted into the memoization caches) runs inside this
        // try. State mutations (depth increment, `logged`, cursor bookkeeping)
        // happen only AFTER the last throwing operation, so an exception leaves the
        // bookkeeping untouched: the frame simply goes untraced and the push below
        // keeps enter/exit pairing intact. The catch body must not allocate.
        //
        // Warm-path allocation budget: zero. Once both caches hold the callee and
        // the call site, everything below runs on stack buffers — resolve() hands
        // back pointers into the caches (ResolvedFrameView), the timestamp is
        // rendered into `timestamp`, and the line is built in `line` and handed
        // to fwrite by length. The previous ResolvedFrame / std::string design
        // cost about three heap allocations per traced call.
        try {
            // Make room for this frame's record FIRST, so the push after the barrier
            // cannot allocate (and so cannot throw). Doubling keeps growth amortized:
            // after warm-up the hot path never allocates. If this reserve throws, the
            // frame becomes an overflow frame (have_slot stays false) and nothing is
            // logged for it — the only way to keep depth accounting exact when there
            // is no record to remember the frame by.
            if (t_state.frames.size() == t_state.frames.capacity()) {
                t_state.frames.reserve(t_state.frames.empty() ? INITIAL_FRAME_CAPACITY
                                                              : t_state.frames.capacity() * 2);
            }
            have_slot = true;

#ifdef LOG_EXCEPTIONS
            // Frames that left without an exit hook (Clang's exception-unwind path
            // runs none, longjmp runs none on either compiler) leave records behind.
            // Reclaim them BEFORE this frame is formatted, so its line lands at the
            // depth of the frames that are really alive. Rules: frameReconcile.h.
            // A dead record found while an exception is in flight (this frame runs
            // from a destructor during unwinding, under Clang) was unwound by that
            // exception; otherwise a non-local jump skipped it.
            resolve_thread_stack_bounds();
            key.level = frame_level(hook_site, hook_frame);
            const char dead_how = std::uncaught_exceptions() > 0 ? utils::FRAME_END_EXCEPTION
                                                                 : utils::FRAME_END_JUMP;
            reclaim_dead_records(instrumentation::dead_records_on_enter(
                                         t_state.frames.data(), t_state.frames.size(), key,
                                         t_state.stack_bounds),
                                 dead_how);
            // Records at this frame's own level (inlined activations, retry loops)
            // need the DWARF inline chain to be told apart — cold, cached per site.
            reclaim_dead_inlined_records_on_enter(key, static_cast<const char*>(hook_site) - 1, dead_how);
#endif

            FILE* fp = get_thread_fp();
            if (fp != nullptr) {
                instrumentation::ResolvedFrameView frame;
                if (instrumentation::resolve(callee, caller, frame)) {
#ifdef LOG_EXCEPTIONS
                    // Pointer into the leaked name cache: valid for the process lifetime.
                    frame_base_name = frame.callee_base_name;
                    // The call site names this frame's direct parent; dead inlined
                    // frames above the parent's record are reclaimed before this
                    // line's depth is fixed.
                    reclaim_dead_inlined_records_above_parent(key, frame.caller_function_base, dead_how);
#endif
                    // Timestamp only for frames that are actually logged; the view
                    // points at this stack buffer for the rest of the hook.
                    char timestamp[utils::PRETTY_TIME_BUF_SIZE];
                    utils::pretty_time_into(timestamp, sizeof(timestamp));
                    frame.timestamp = timestamp;
                    char line[utils::FORMAT_BUF_SIZE];
                    // Formatted with the prospective depth; the actual increment
                    // follows in the no-throw zone. The '\n' lands in the same
                    // stack buffer, so the line is ready to write as-is. With
                    // LOG_ELAPSED the fixed-width "[  pending ] " placeholder is
                    // spliced right after the timestamp (see include/durationFormat.h
                    // for the width invariant).
                    const std::size_t line_size =
                            utils::format_into(line, sizeof(line), frame,
                                               t_state.current_stack_depth + 1,
                                               CSLG_DURATION_SPLICE, /*append_newline=*/true);
                    // ---- no-throw zone ----
                    t_state.current_stack_depth++;
                    logged = true;
#ifdef CSLG_LINE_PATCHING
                    // Remember where this line starts: every patch offset of the
                    // frame's record derives from it (recorded after the push below).
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
                    if (fwrite(line, 1, line_size, fp) == line_size) {
                        t_state.cursor += static_cast<off_t>(line_size);
                    } else {
                        t_state.cursor_valid = false;
                    }
#else
                    // No mutex: this FILE* is private to this thread. One fwrite of
                    // the newline-terminated line — a single stdio call with no
                    // per-line format-string parsing, mirroring the patching
                    // branch minus its cursor bookkeeping.
                    (void)fwrite(line, 1, line_size, fp);
#endif
                }
            }
        } catch (...) {
            // Swallow (realistically only bad_alloc under OOM). The frame goes
            // untraced; `logged` stayed false and no state was half-updated.
        }

        if (!have_slot) {
            // Growing the frame stack failed: no record can remember this frame, so
            // it is tracked by count only (see frame_overflow_count). Nothing was
            // logged for it, so depth accounting stays exact.
            t_state.frame_overflow_count++;
        } else {
            // Push a frame record for EVERY call — even when nothing was logged (trace
            // not ready, open failed, resolution filtered the frame, or shutdown
            // completed). Pairing with __cyg_profile_func_exit must depend only on call
            // structure: the exit hook pops unconditionally, so gating this push on fp
            // availability would desynchronize the stack whenever availability changes
            // between a frame's enter and its exit. Concretely, enters that skipped
            // pushing after shutdown (or before trace_ready, on a thread spawned by an
            // instrumented static constructor) shifted every subsequent pop onto an
            // ancestor's slot — and, with LOG_ELAPSED, patched the wrong line's
            // duration field.
            FrameRecord record{};
            record.logged = logged;
#ifdef LOG_EXCEPTIONS
            record.callee = key.callee;
            record.caller = key.caller;
            // Null only if the barrier above was left before the level was derived
            // (then the record is an unlogged one whose level nothing will match).
            record.level = key.level;
            record.name_base = logged ? frame_base_name : nullptr;
            record.uncaught_at_enter = std::uncaught_exceptions();
#endif
#ifdef CSLG_LINE_PATCHING
            record.line_start = -1;
            record.depth = 0;
            if (logged && t_state.cursor_valid) {
                // Only when the cursor is still trustworthy (this line's write
                // included — a failure in it invalidated the cursor above); with
                // an untrusted cursor, keep the -1 sentinel so no patch is ever
                // written for this frame (its placeholder stays "[  pending ]").
                record.line_start = line_start;
                record.depth = t_state.current_stack_depth;
            }
#endif
#ifdef LOG_ELAPSED
            if (logged) {
                // Read the clock as the hook's LAST step for this frame, mirrored
                // by the exit hook reading it FIRST (before its own format +
                // pwrite work): the frame's reported span covers the function
                // body but not the tracer's own per-call work (resolve + format +
                // line write — microseconds even when the memoization caches hit,
                // tens of microseconds cold). Hook overhead of calls nested
                // INSIDE the function still lands in the parent's span —
                // unavoidable without per-frame overhead accounting.
                record.enter_time = std::chrono::steady_clock::now();
            }
#endif
            // Capacity was reserved inside the barrier and FrameRecord is trivially
            // copyable, so this push cannot reallocate or throw.
            t_state.frames.push_back(record);
        }
    }
    t_in_instrumentation = false;
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_exit(void *callee, void *caller) {
    // The exit hook needs neither address: pairing is positional (LIFO pop of the
    // per-thread frame stack). The parameters exist because the compiler-emitted
    // call passes them; silence -Wextra's unused-parameter warning.
    (void)callee;
    (void)caller;
    if (t_in_instrumentation) { return; }
#if defined(LOG_ELAPSED) || defined(LOG_EXCEPTIONS)
    // Set the re-entrancy guard because below we call std::chrono::steady_clock::now(),
    // pwrite() and std::uncaught_exceptions() — calls that are safe in trace.cpp
    // (compiled without instrumentation) but want protection from any exotic indirect
    // instrumentation path. pwrite is also a cancellation point, so cancellation is
    // blocked (see ScopedCancelDisable). Without either option the exit handler is
    // mutex-free and I/O-free, so we keep the zero-overhead guarantee by skipping
    // these stores entirely.
    t_in_instrumentation = true;
    ScopedCancelDisable no_cancel;
#endif
    // Pop a frame record for EVERY call — the exact mirror of the unconditional push
    // in __cyg_profile_func_enter (see the comment there). Deliberately NO fp check:
    // everything touched here is thread_local, and gating pops on fp availability is
    // precisely what desynchronized the pairing across the shutdown boundary (enters
    // stopped pushing once shutdown_complete was set, while exits kept popping
    // against the still-open raw FILE*, shifting pops onto ancestors' slots).
    if (t_state.frame_overflow_count > 0) {
        // This exit belongs to an overflow frame (see frame_overflow_count). It was
        // never logged, so it never touched current_stack_depth: just count it down.
        // Overflow frames always sit above every pushed record, so draining the
        // counter before popping keeps the pairing exact.
        t_state.frame_overflow_count--;
    } else if (!t_state.frames.empty()) {
#ifdef LOG_EXCEPTIONS
        // Find THIS frame's record instead of taking the top one blindly: records
        // above it belong to frames that left without an exit hook (frameReconcile.h).
        // `tail_called` tells whether the compiler turned this hook call into a jump
        // (both do at -O2 for void functions): the frame is already gone then and the
        // level resolves to the caller's, which the rule accounts for. frame_level()
        // never throws, so no barrier is needed here.
        const void* const hook_site = __builtin_return_address(0);
        const instrumentation::FrameKey key{ callee, caller,
                                             frame_level(hook_site, __builtin_frame_address(0)) };
        const bool tail_called = hook_site == caller;
        const std::size_t index = instrumentation::exiting_record_index(
                t_state.frames.data(), t_state.frames.size(), key, tail_called, t_state.stack_bounds);
        reclaim_dead_records(t_state.frames.size() - 1 - index, utils::FRAME_END_JUMP);
#endif
        // Copy the record out BEFORE popping so LOG_ELAPSED patches the very line
        // the enter handler wrote for this frame. pop_back() never shrinks the
        // vector's capacity, so the hot path stays allocation-free.
        const FrameRecord record = t_state.frames.back();
        t_state.frames.pop_back();
        if (record.logged) {
            t_state.current_stack_depth--;
#ifdef LOG_EXCEPTIONS
            // An exception is leaving this frame when more exceptions are in flight
            // now than when it was entered (see FrameRecord::uncaught_at_enter).
            const bool by_exception = std::uncaught_exceptions() > record.uncaught_at_enter;
#endif
#ifdef LOG_ELAPSED
            // Compute elapsed and patch the matching line's placeholder.
            const auto exit_time = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    exit_time - record.enter_time).count();
            // steady_clock is monotonic so elapsed shouldn't go negative,
            // but clamp defensively so the uint64_t cast stays safe across
            // any future clock-source quirks.
            const std::uint64_t ns = (elapsed < 0) ? 0 :
                    static_cast<std::uint64_t>(elapsed);
            char buf[utils::DURATION_FIELD_WIDTH + 1];
            utils::format_duration_12chars(ns, buf);
#ifdef LOG_EXCEPTIONS
            if (by_exception) {
                utils::set_duration_flag(buf, utils::FRAME_END_EXCEPTION);
            }
#endif
            patch_line_bytes(record.line_start, PLACEHOLDER_OFFSET_IN_LINE, buf,
                             utils::DURATION_FIELD_WIDTH);
#endif
#ifdef LOG_EXCEPTIONS
            if (by_exception) {
                // The duration (if any) is already in place; only the glyph is left.
                mark_frame_end(record, utils::FRAME_END_EXCEPTION, nullptr);
            }
#endif
        }
    }
#if defined(LOG_ELAPSED) || defined(LOG_EXCEPTIONS)
    t_in_instrumentation = false;
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
