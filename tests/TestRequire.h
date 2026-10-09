#pragma once

#include <cstdlib>
#include <iostream>

namespace SpoolTests {

// Test selectors run in isolated children, so a failed requirement terminates
// only its selector and leaves the supervisor free to collect sibling results.
inline void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace SpoolTests
