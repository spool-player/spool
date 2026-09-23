#include "common/RequestGeneration.h"

#include "TestMain.h"

#include <cstdlib>
#include <iostream>

using Spool::RequestGeneration;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

SPOOL_TEST_MAIN("request-generation")
{
    RequestGeneration generation;
    const RequestGeneration::Token first = generation.next();
    require(!generation.isCurrent(generation.current() - 1),
        "an issued token must not share a generation with the one before it");
    require(generation.isCurrent(first), "the newest token should own the current generation");
    require(generation.current() == first, "current() should report the newest issued token");

    const RequestGeneration::Token second = generation.next();
    require(second != first, "each request should receive a distinct token");
    require(!generation.isCurrent(first), "a superseded request must not deliver its result");
    require(generation.isCurrent(second), "the newest token should own the current generation");

    generation.invalidate();
    require(!generation.isCurrent(second), "invalidation should discard the in-flight request");
    require(!generation.isCurrent(first), "invalidation must not resurrect an older generation");
    require(generation.isCurrent(generation.next()), "a generation stays usable after invalidation");

    return EXIT_SUCCESS;
}
