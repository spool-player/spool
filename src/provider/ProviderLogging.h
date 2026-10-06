#pragma once

#include <QString>

namespace Spool {

// Provider diagnostics use the application Qt sink and its native category
// rules. Trace is a separate, default-disabled QtDebug category.
enum class ProviderLogLevel { Trace, Debug, Info, Warn, Error };
bool providerLogEnabled(ProviderLogLevel level);
void writeProviderLog(ProviderLogLevel level, const QString& message);

} // namespace Spool
