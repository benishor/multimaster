// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

// Minimal dependency-free test harness. Keeps the project zero-dependency.
//
//   TEST("name") { CHECK(cond); CHECK_EQ(a, b); }
//   int main() { return mm::test::run(); }

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mm::test {

struct Case {
    std::string           name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

inline int run() {
    int passed = 0;
    for (auto& c : registry()) {
        int before = failures();
        c.fn();
        if (failures() == before) {
            ++passed;
            std::printf("  ok   %s\n", c.name.c_str());
        } else {
            std::printf("  FAIL %s\n", c.name.c_str());
        }
    }
    std::printf("\n%d/%zu cases passed\n", passed, registry().size());
    return failures() == 0 ? 0 : 1;
}

} // namespace mm::test

#define MM_CONCAT_(a, b) a##b
#define MM_CONCAT(a, b) MM_CONCAT_(a, b)

#define TEST(name)                                                                  \
    static void MM_CONCAT(mm_test_fn_, __LINE__)();                                  \
    static ::mm::test::Registrar MM_CONCAT(mm_test_reg_, __LINE__)(                  \
        name, &MM_CONCAT(mm_test_fn_, __LINE__));                                    \
    static void MM_CONCAT(mm_test_fn_, __LINE__)()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++::mm::test::failures();                                               \
            std::printf("    CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        if (!((a) == (b))) {                                                        \
            ++::mm::test::failures();                                               \
            std::printf("    CHECK_EQ failed: %s == %s (%s:%d)\n", #a, #b,          \
                        __FILE__, __LINE__);                                        \
        }                                                                           \
    } while (0)
