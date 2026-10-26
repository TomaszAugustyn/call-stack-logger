/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

#include <cstddef>
#include <cstdint>

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

// A direct-mapped cache in front of a shared hash map (pure logic, unit-tested).
//
// The resolver memoizes every address it has seen in hash maps that all threads
// share under one mutex (callStack.h). On the warm path — a traced call whose
// callee and call site were seen before — that mutex was held for two hash
// lookups and nothing else, and still the threads of a program tracing at full
// speed collided on it: std::mutex sleeps the moment it finds the lock taken,
// and with a critical section of about 100 ns the sleeping and waking cost more
// than the work (a lock convoy). Measured on the reference host, the cost per
// traced call rose from 3.3 µs with one thread to 5.3 µs with eight, and a
// thread's first lookup in a large library (about 20 ms) stalled every other
// thread's warm calls for as long.
//
// Each thread therefore keeps a FrontCache per shared map: SLOTS entries of
// (key, pointer to the shared map's node), indexed by a hash of the key. A hit
// costs one compare and no lock; a miss, or a slot holding another key, goes
// the locked way and the slot is refilled. Storing POINTERS to the nodes is
// what makes this safe without any synchronization: the shared maps are leaked,
// node-based and never erased from or modified (see name_cache() in
// callStack.h), so a node pointer stays valid for the process lifetime — the
// property ResolvedFrameView relies on already — and a slot never needs
// invalidating. The entries are plain data, so a thread_local FrontCache is
// zero-initialized in TLS and never destroyed: no constructor can run too
// late and no destructor too early (the hooks can run after a thread's other
// thread_local objects are gone).
namespace instrumentation {

/// `Value` is the shared map's mapped type; `SLOTS` a power of two, at least 2.
/// 1024 slots of 16 bytes cost 16 KB per thread.
template <typename Value, std::size_t SLOTS>
struct FrontCache {
    static_assert(SLOTS >= 2 && (SLOTS & (SLOTS - 1)) == 0, "SLOTS must be a power of two");

    struct Entry {
        const void* key;
        const Value* value;
    };

    Entry entries[SLOTS];

    /// The slot of `key`: the top bits of the key times the 64-bit golden-ratio
    /// constant (Fibonacci hashing). Code addresses that are close to each
    /// other — the call sites of one function, the functions of one object —
    /// differ in their low bits only, and a plain modulus would put the ones
    /// that are a multiple of SLOTS apart into the same slot; the multiplication
    /// spreads them.
    NO_INSTRUMENT
    static std::size_t slot_of(const void* key) {
        constexpr std::uint64_t GOLDEN_RATIO_64 = 0x9E3779B97F4A7C15ull;
        constexpr unsigned SLOT_BITS = bits_of(SLOTS);
        const std::uint64_t mixed
                = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key)) * GOLDEN_RATIO_64;
        return static_cast<std::size_t>(mixed >> (64 - SLOT_BITS));
    }

    /// The value remembered for `key`, or nullptr when the slot holds another
    /// key or nothing yet. A value is never null, so nullptr always means a miss.
    NO_INSTRUMENT
    const Value* find(const void* key) const {
        const Entry& entry = entries[slot_of(key)];
        return entry.key == key ? entry.value : nullptr;
    }

    /// Remembers `value` for `key`, whatever the slot held before.
    NO_INSTRUMENT
    void store(const void* key, const Value* value) {
        Entry& entry = entries[slot_of(key)];
        entry.key = key;
        entry.value = value;
    }

private:
    /// log2 of a power of two.
    NO_INSTRUMENT
    static constexpr unsigned bits_of(std::size_t power_of_two) {
        return power_of_two <= 1 ? 0 : 1 + bits_of(power_of_two / 2);
    }
};

} // namespace instrumentation
