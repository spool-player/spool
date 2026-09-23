#pragma once

// Tests are grouped into one executable per directory rather than one
// executable per test. Every test still gets its own ctest entry: the shared
// runner in TestRunner.cpp dispatches on the selector that add_test() passes
// as the first argument. Grouping matters most on Windows, where each test
// executable otherwise pays a whole-program-optimized link against the entire
// spool-core static library.
//
// Write a test exactly as a standalone program, but name its entry point with
// SPOOL_TEST_MAIN("selector") instead of main(). Keep helpers in an
// anonymous namespace so sibling tests in the same binary cannot collide.

namespace SpoolTests {

using Entry = int (*)(int argc, char **argv);

bool registerTest(const char *name, Entry entry);

} // namespace SpoolTests

#define SPOOL_TEST_MAIN(selector)                                                                                      \
    static int spoolTestBody([[maybe_unused]] int argc, [[maybe_unused]] char **argv);                                 \
    namespace {                                                                                                        \
        [[maybe_unused]] const bool spoolTestRegistered = ::SpoolTests::registerTest(selector, &spoolTestBody);        \
    }                                                                                                                  \
    static int spoolTestBody([[maybe_unused]] int argc, [[maybe_unused]] char **argv)
