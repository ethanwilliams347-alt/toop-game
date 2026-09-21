#pragma once
#include <cstdio>
#include <string>

// Shared by every test executable. There is still no test framework by design:
// a failing test is a non-zero exit code, which is all CTest needs. This header
// exists only so a second test binary does not mean a second copy of the same
// six lines.

inline int test_failures = 0;

inline void check(const char* name, bool ok, const std::string& detail = "") {
    std::printf("%s %-50s %s\n", ok ? "[PASS]" : "[FAIL]", name, detail.c_str());
    if (!ok) test_failures++;
}

// Use as `return report();` from main().
inline int report() {
    std::printf("\n%s (%d failure%s)\n", test_failures ? "FAILURES" : "ALL PASS",
                test_failures, test_failures == 1 ? "" : "s");
    return test_failures ? 1 : 0;
}
