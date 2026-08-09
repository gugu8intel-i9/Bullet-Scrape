#include "test_harness.hpp"
#include <cstring>

int main(int argc, char** argv) {
    std::string filter;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
            filter = argv[++i];

    auto& tests = test_registry();
    if (tests.empty()) {
        std::cerr << "No tests registered.\n";
        return 2;
    }

    int passed = 0, failed = 0, skipped = 0;
    for (auto& [name, fn] : tests) {
        if (!filter.empty() && name != filter && filter != "all") {
            skipped++;
            continue;
        }
        try {
            bool ok = fn();
            if (ok) { passed++; std::cout << "  PASS " << name << "\n"; }
            else   { failed++; std::cout << "  FAIL " << name << "\n"; }
        } catch (const std::exception& e) {
            failed++;
            std::cout << "  FAIL " << name << " — " << e.what() << "\n";
        }
    }

    std::cout << "\nResults: " << passed << " passed"
              << ", " << failed << " failed"
              << ", " << skipped << " skipped"
              << " (of " << tests.size() << " total)\n";
    return failed > 0 ? 1 : 0;
}
