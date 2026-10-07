#pragma once

// One native binary owns traditional selectors and one owns GUI e2e selectors.
// The shared runner supervises isolated SAME-binary subprocesses so a test may
// own its Q(Core/Gui)Application, environment and exit() without harming peers.
// Use --list, --child SELECTOR, or --run-all --workers N --results FILE.
//
// Write a test exactly as a standalone program, but name its entry point with
// SPOOL_TEST_MAIN("selector") instead of main(). Keep helpers in an
// anonymous namespace so sibling tests in the same binary cannot collide.

namespace SpoolTests {

using Entry = int (*)(int argc, char **argv);

bool registerTest(const char *name, Entry entry);
int invoke(const char *name, int argc, char **argv);

} // namespace SpoolTests

#define SPOOL_TEST_MAIN(selector)                                                                                      \
    static int spoolTestBody([[maybe_unused]] int argc, [[maybe_unused]] char **argv);                                 \
    namespace {                                                                                                        \
        [[maybe_unused]] const bool spoolTestRegistered = ::SpoolTests::registerTest(selector, &spoolTestBody);        \
    }                                                                                                                  \
    static int spoolTestBody([[maybe_unused]] int argc, [[maybe_unused]] char **argv)
