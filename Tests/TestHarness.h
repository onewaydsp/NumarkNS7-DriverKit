// TestHarness.h
// Tiny self-registering test harness shared by the host unit tests in Tests/.
// Each test file defines cases with TEST(name) { ... }; TestMain.cpp runs
// them in registration order (file link order, then definition order).

#ifndef TestHarness_h
#define TestHarness_h

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace test {

inline int gFailures = 0;
inline int gChecks   = 0;

struct Case { const char * name; void (*fn)(); };

inline std::vector<Case> & Registry()
{
    static std::vector<Case> cases;
    return cases;
}

struct Register {
    Register(const char * name, void (*fn)()) { Registry().push_back({ name, fn }); }
};

// Seed for the randomized tests: fixed by default so runs are reproducible;
// override with NS7_TEST_SEED=<n> to explore. TestMain logs the value.
inline uint32_t Seed()
{
    static const uint32_t seed = [] {
        const char * s = std::getenv("NS7_TEST_SEED");
        return s ? uint32_t(std::strtoul(s, nullptr, 0)) : 0x4E533700u;   // "NS7\0"
    }();
    return seed;
}

} // namespace test

using test::gChecks;
using test::gFailures;

#define TEST(name)                                                             \
    static void name();                                                        \
    static ::test::Register name##_registration(#name, name);                  \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        gChecks++;                                                             \
        if (!(cond)) {                                                         \
            gFailures++;                                                       \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        gChecks++;                                                             \
        auto _a = (a); auto _b = (b);                                          \
        if (!(_a == _b)) {                                                     \
            gFailures++;                                                       \
            std::printf("  FAIL %s:%d: %s == %s  (0x%llx vs 0x%llx)\n",        \
                        __FILE__, __LINE__, #a, #b,                            \
                        (unsigned long long)_a, (unsigned long long)_b);       \
        }                                                                      \
    } while (0)

#endif // TestHarness_h
