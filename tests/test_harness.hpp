#pragma once
#include <vector>
#include <string>
#include <utility>
#include <iostream>

// ── Test registry ────────────────────────────────────────────────────────────
// Uses inline (C++17) so the function has a single definition across all
// translation units — no ODR violation.

inline std::vector<std::pair<const char*, bool(*)()>>& test_registry() {
    static std::vector<std::pair<const char*, bool(*)()>> tests;
    return tests;
}

// ── REGISTER_TEST macro ──────────────────────────────────────────────────────
// Each test function is declared, then a static struct with a constructor
// registers it in the global registry. The struct and variable names include
// the test name to avoid collisions.

#define REGISTER_TEST(name) \
    static bool test_##name(); \
    struct register_test_##name { \
        register_test_##name() { \
            test_registry().push_back({#name, test_##name}); \
        } \
    }; \
    static register_test_##name register_test_var_##name; \
    static bool test_##name()
