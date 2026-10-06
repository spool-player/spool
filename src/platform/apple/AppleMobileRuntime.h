#pragma once

namespace Spool {
// Render-thread-safe gate: Apple forbids GPU work after backgrounding.
bool appleMobileRenderingAllowed();
} // namespace Spool
