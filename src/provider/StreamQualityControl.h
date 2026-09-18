#pragma once

#include <QString>
#include <QtTypes>

#include <vector>

namespace JellyfinNative {

// A source that can deliver a stream below its stored quality, so the player
// can offer a ladder and remember a ceiling for the session. Plain abstract
// class, see Catalog. A source that always plays the file as stored has no
// StreamQuality capability and no object here.
class StreamQualityControl {
public:
    struct Rung {
        QString label;
        qint64 bitrate = 0;
        int height = 0;
    };

    virtual ~StreamQualityControl() = default;

    // The ceiling the viewer picked for this session; zero means Auto.
    virtual qint64 bitrateOverride() const = 0;
    virtual int heightOverride() const = 0;
    virtual void setOverride(qint64 bitrate, int height) = 0;
    // The line under "Auto": what the automatic ceiling currently is and
    // where it came from.
    virtual QString autoDescription() const = 0;
    // The ceilings worth offering below a stream of this bitrate, coarsest
    // first.
    virtual std::vector<Rung> ladder(qint64 sourceBitrate) const = 0;
};

} // namespace JellyfinNative
