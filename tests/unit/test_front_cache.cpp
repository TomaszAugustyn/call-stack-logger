/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Unit tests for instrumentation::FrontCache in include/frontCache.h: the
 * per-thread direct-mapped cache the resolver keeps in front of its shared
 * lookup tables. Pure logic — the tests use it with plain integers as the
 * values and made-up addresses as the keys.
 */

#include "frontCache.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <set>

namespace {

using Cache = instrumentation::FrontCache<int, 1024>;

const void* address(std::uintptr_t value) {
    return reinterpret_cast<const void*>(value);
}

// Two keys that land in the same slot: the first key, and the first later key
// with the same slot.
std::pair<const void*, const void*> colliding_keys() {
    const void* first = address(0x401000);
    for (std::uintptr_t candidate = 0x401008;; candidate += 8) {
        if (Cache::slot_of(address(candidate)) == Cache::slot_of(first)) {
            return { first, address(candidate) };
        }
    }
}

} // namespace

TEST(FrontCacheTest, EmptyCacheMissesEveryKey) {
    Cache cache{};
    EXPECT_EQ(cache.find(address(0x401000)), nullptr);
    EXPECT_EQ(cache.find(nullptr), nullptr) << "a null key must never hit a zero-initialized slot";
}

TEST(FrontCacheTest, StoredValueIsFoundAgain) {
    Cache cache{};
    const int value = 7;
    cache.store(address(0x401000), &value);
    EXPECT_EQ(cache.find(address(0x401000)), &value);
    EXPECT_EQ(cache.find(address(0x401001)), nullptr) << "a different key must not hit";
}

TEST(FrontCacheTest, KeysInDifferentSlotsCoexist) {
    Cache cache{};
    const int a = 1;
    const int b = 2;
    const void* key_a = address(0x401000);
    const void* key_b = address(0x401010);
    ASSERT_NE(Cache::slot_of(key_a), Cache::slot_of(key_b));
    cache.store(key_a, &a);
    cache.store(key_b, &b);
    EXPECT_EQ(cache.find(key_a), &a);
    EXPECT_EQ(cache.find(key_b), &b);
}

TEST(FrontCacheTest, AnotherKeyInTheSameSlotMissesAndStoringItReplacesTheFirst) {
    Cache cache{};
    const int a = 1;
    const int b = 2;
    const auto [key_a, key_b] = colliding_keys();
    cache.store(key_a, &a);
    EXPECT_EQ(cache.find(key_b), nullptr) << "the slot holds another key";
    cache.store(key_b, &b);
    EXPECT_EQ(cache.find(key_b), &b);
    EXPECT_EQ(cache.find(key_a), nullptr) << "the slot was refilled with the other key";
}

TEST(FrontCacheTest, SlotsStayInRange) {
    for (std::uintptr_t key = 0; key < 0x10000; key += 0x1ff) {
        EXPECT_LT(Cache::slot_of(address(key)), 1024u);
    }
    EXPECT_LT(Cache::slot_of(address(UINTPTR_MAX)), 1024u);
}

// Call sites of one function differ by a few bytes, functions of one object by
// a few dozen: keys close to each other must spread over the slots instead of
// sharing a few. 256 keys 8 bytes apart in 1024 slots: at most a handful of
// collisions with a good hash, almost all in one slot with a bad one.
TEST(FrontCacheTest, CloseKeysSpreadOverTheSlots) {
    std::set<std::size_t> slots;
    for (std::uintptr_t key = 0x401000; key < 0x401000 + 256 * 8; key += 8) {
        slots.insert(Cache::slot_of(address(key)));
    }
    EXPECT_GE(slots.size(), 200u) << "only " << slots.size() << " distinct slots for 256 close keys";
}
