#pragma once

#include <QString>

namespace Spool {

struct MpvConfigPolicy {
    enum class Mode {
        Disabled,
        Standard,
        Custom,
    };

    Mode mode = Mode::Disabled;
    QString directory;
    bool valid = true;
    QString error;
    bool operator==(const MpvConfigPolicy&) const = default;
};

MpvConfigPolicy validatedPlatformMpvConfigPolicy(const QString& mode, const QString& directory);

} // namespace Spool
