#pragma once

namespace SpoolTests {

// Materialize packaged fixtures before a selector constructs its application.
// Tests use the same filesystem paths on desktop and in the mobile sandbox.
bool prepareMobileFixtures();

} // namespace SpoolTests
