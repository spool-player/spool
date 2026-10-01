#pragma once

#include <QString>

namespace Spool {

class AndroidUpdateInstaller final {
public:
    static bool canRequestPackageInstalls();
    static bool install(const QString& packagePath);
    static bool openInstallSettings();
};

} // namespace Spool
