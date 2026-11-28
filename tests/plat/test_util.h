// Minimal dependency-free test harness shared by the unit tests and the
// selftest: CHECK records a failure and continues, results print TAP-style.
#pragma once

#include <cstdio>
#include <functional>
#include <string>

namespace plat_test {

struct Result {
    int pass = 0, fail = 0, skip = 0;
};
inline Result      g_result;
inline bool        g_caseFailed = false;
inline bool        g_skipped    = false;
inline std::string g_skipWhy;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("    check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            plat_test::g_caseFailed = true;                                                        \
        }                                                                                          \
    } while (0)

// Mark the running case skipped (it should return right after).
inline void skip(std::string why) {
    g_skipped = true;
    g_skipWhy = std::move(why);
}

inline void runCase(const char *name, const std::function<void()> &fn) {
    g_caseFailed = false;
    g_skipped    = false;
    fn();
    if (g_caseFailed) {
        ++g_result.fail;
        std::printf("FAIL %s\n", name);
    } else if (g_skipped) {
        ++g_result.skip;
        std::printf("SKIP %s: %s\n", name, g_skipWhy.c_str());
    } else {
        ++g_result.pass;
        std::printf("PASS %s\n", name);
    }
    std::fflush(stdout);
}

inline int summary() {
    std::printf(
        "\n%d passed, %d failed, %d skipped\n", g_result.pass, g_result.fail, g_result.skip
    );
    std::fflush(stdout);
    return g_result.fail ? 1 : 0;
}

} // namespace plat_test
