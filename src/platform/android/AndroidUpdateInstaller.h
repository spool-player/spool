#pragma once

#include <QString>

namespace JellyfinNative {

class AndroidUpdateInstaller final {
public:
    static bool canRequestPackageInstalls();
    static bool install(const QString& packagePath);
    static bool openInstallSettings();
};

} // namespace JellyfinNative
