#include "TestMain.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

namespace SpoolTests {

namespace {

    std::map<std::string, Entry>& registry()
    {
        static std::map<std::string, Entry> tests;
        return tests;
    }

} // namespace

bool registerTest(const char *name, Entry entry)
{
    registry().emplace(name, entry);
    return true;
}

} // namespace SpoolTests

int main(int argc, char **argv)
{
    const auto& tests = SpoolTests::registry();
    const auto usage = [&tests](const char *problem) {
        std::cerr << problem << "\nusage: " << "<test-executable> <selector>\navailable selectors:\n";
        for (const auto& [name, entry] : tests)
            std::cerr << "  " << name << '\n';
        return EXIT_FAILURE;
    };

    if (argc < 2)
        return usage("no test selector was given");
    const auto selected = tests.find(argv[1]);
    if (selected == tests.end())
        return usage("no test is registered under that selector");

    // Hand the test the argument vector it would have seen as its own program:
    // the selector takes the place of argv[0] so QCoreApplication and friends
    // still see a valid program name followed by any extra arguments.
    argv[1] = argv[0];
    return selected->second(argc - 1, argv + 1);
}
