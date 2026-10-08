#pragma once

#include <QString>

namespace SpoolTests {

// Materialize packaged fixtures before a selector constructs its application.
// Tests use the same filesystem paths on desktop and in the mobile sandbox.
bool prepareMobileFixtures();
const QString& mobileFixtureRoot();

} // namespace SpoolTests
