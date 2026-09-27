// TestMain.cpp
// Runs every TEST registered by the Tests/*.cpp files. Run with:  make -C Tests
// Optional argument: a substring; only tests whose name contains it run.

#include "TestHarness.h"

#include <cstring>

int main(int argc, char ** argv)
{
    const char * filter = argc > 1 ? argv[1] : nullptr;
    std::printf("random seed 0x%08x (set NS7_TEST_SEED to override)\n", test::Seed());
    int ran = 0;
    for (const test::Case & t : test::Registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        const int before = gFailures;
        t.fn();
        ran++;
        std::printf("%s %s\n", gFailures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("\n%d tests, %d checks, %d failures\n", ran, gChecks, gFailures);
    return gFailures == 0 && ran > 0 ? 0 : 1;
}
