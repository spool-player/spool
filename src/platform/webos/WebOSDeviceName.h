#pragma once

#include <QString>

#include <functional>

namespace Spool {

// The set's user-visible name, which webOS only reports asynchronously. Every
// install would otherwise register with the same hardcoded name and be
// indistinguishable in a client list.
//
// The callback runs whenever the name is known, including again when the user
// renames the television in Settings, and never when it cannot be read.
void requestWebOSDeviceName(std::function<void(const QString&)> callback);

} // namespace Spool
