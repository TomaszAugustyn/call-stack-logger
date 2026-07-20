/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Unit tests for instrumentation::is_std_library_symbol() in
 * include/stdSymbolFilter.h — the Clang-only runtime std-library filter.
 *
 * The function is pure Itanium-ABI mangled-name parsing, so it is compiled and
 * tested on both compilers even though callStack.cpp only consults it under
 * Clang. Each documented pattern gets a positive case with a real-world mangled
 * name; the negatives pin down user symbols that must NEVER be filtered.
 */

#include "stdSymbolFilter.h"
#include <gtest/gtest.h>

using instrumentation::is_std_library_symbol;

// -------- C++ ABI runtime (__cxa_* — no _Z prefix) --------

TEST(StdSymbolFilterTest, CxaRuntimeFunctionsFiltered) {
    EXPECT_TRUE(is_std_library_symbol("__cxa_atexit"));
    EXPECT_TRUE(is_std_library_symbol("__cxa_guard_acquire"));
    EXPECT_TRUE(is_std_library_symbol("__cxa_throw"));
}

// -------- std:: proper ('St', with and without nesting / cv-qualifiers) --------

TEST(StdSymbolFilterTest, StdFreeFunctionFiltered) {
    // std::sort<...> — free function: _ZSt...
    EXPECT_TRUE(is_std_library_symbol("_ZSt4sortIN9__gnu_cxx17__normal_iteratorIPiSt6vectorIiSaIiEEEEEvT_S7_"));
}

TEST(StdSymbolFilterTest, StdMemberFunctionFiltered) {
    // std::vector<int>::push_back — nested name: _ZNSt...
    EXPECT_TRUE(is_std_library_symbol("_ZNSt6vectorIiSaIiEE9push_backEOi"));
}

TEST(StdSymbolFilterTest, ConstQualifiedStdMemberFiltered) {
    // std::vector<int>::size() const — 'K' after 'N': _ZNKSt...
    EXPECT_TRUE(is_std_library_symbol("_ZNKSt6vectorIiSaIiEE4sizeEv"));
}

TEST(StdSymbolFilterTest, VolatileQualifiedStdMemberFiltered) {
    // 'V' cv-qualifier after 'N'.
    EXPECT_TRUE(is_std_library_symbol("_ZNVSt6atomicIiE5storeEi"));
}

TEST(StdSymbolFilterTest, LocalEntityInsideStdFunctionFiltered) {
    // _ZZ prefix: local entity (e.g. a _Guard class) inside a std:: function.
    EXPECT_TRUE(is_std_library_symbol("_ZZNSt6vectorIiSaIiEE17_M_realloc_insertEvE6_Guard"));
}

// -------- Standard substitutions (Sa / Sb / Ss / Si / So / Sd) --------

TEST(StdSymbolFilterTest, StandardSubstitutionsFiltered) {
    // Sa = std::allocator (constructor).
    EXPECT_TRUE(is_std_library_symbol("_ZNSaIcEC1Ev"));
    // Sb = std::basic_string<wchar_t, ...>.
    EXPECT_TRUE(is_std_library_symbol("_ZNSbIwSt11char_traitsIwESaIwEE4sizeEv"));
    // Ss = std::string.
    EXPECT_TRUE(is_std_library_symbol("_ZNSs6appendEPKc"));
    // So = std::ostream member.
    EXPECT_TRUE(is_std_library_symbol("_ZNSo5writeEPKcl"));
}

// -------- GNU extensions and ABI internals --------

TEST(StdSymbolFilterTest, GnuCxxExtensionsFiltered) {
    EXPECT_TRUE(is_std_library_symbol(
            "_ZN9__gnu_cxx17__normal_iteratorIPiSt6vectorIiSaIiEEEppEv"));
}

TEST(StdSymbolFilterTest, CxxAbiInternalsFiltered) {
    EXPECT_TRUE(is_std_library_symbol("_ZN10__cxxabiv117__class_type_infoD0Ev"));
}

TEST(StdSymbolFilterTest, GnuDebugContainersFiltered) {
    EXPECT_TRUE(is_std_library_symbol("_ZN11__gnu_debug16_Error_formatter8_M_errorEv"));
}

// -------- Negatives: user symbols must never be filtered --------

TEST(StdSymbolFilterTest, PlainCFunctionNotFiltered) {
    EXPECT_FALSE(is_std_library_symbol("main"));
    EXPECT_FALSE(is_std_library_symbol("worker_top"));
}

TEST(StdSymbolFilterTest, UserFreeFunctionNotFiltered) {
    // func_a() — a length-prefixed user name after _Z.
    EXPECT_FALSE(is_std_library_symbol("_Z6func_av"));
}

TEST(StdSymbolFilterTest, UserNamespaceAndClassNotFiltered) {
    EXPECT_FALSE(is_std_library_symbol("_ZN4user3fooEv"));
}

TEST(StdSymbolFilterTest, UserNameStartingWithCapitalSNotFiltered) {
    // A user function named "Stop" mangles as _Z4Stopv — the length digit
    // before 'S' must prevent a false match against the substitution check.
    EXPECT_FALSE(is_std_library_symbol("_Z4Stopv"));
}

TEST(StdSymbolFilterTest, UserClassNamedStNotFiltered) {
    // A user class literally named "St" mangles with a length prefix ("2St"),
    // unlike the std:: abbreviation which has no digit.
    EXPECT_FALSE(is_std_library_symbol("_ZN2St3fooEv"));
}

TEST(StdSymbolFilterTest, GnuLikeNameNestedInUserNamespaceNotFiltered) {
    // "__gnu_cxx" appearing deeper in the name (not as the first component)
    // must not trigger the prefix checks.
    EXPECT_FALSE(is_std_library_symbol("_ZN6mySpace9__gnu_cxxE"));
}

TEST(StdSymbolFilterTest, EmptyAndMalformedInputsNotFiltered) {
    EXPECT_FALSE(is_std_library_symbol(""));
    EXPECT_FALSE(is_std_library_symbol("_"));
    EXPECT_FALSE(is_std_library_symbol("_Y1x"));
}
