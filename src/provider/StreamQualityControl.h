#pragma once

#include <QString>
#include <QtTypes>

#include <cmath>
#include <iterator>
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
    // The ceilings worth offering below a stream of this bitrate and height,
    // coarsest first.
    virtual std::vector<Rung> ladder(qint64 sourceBitrate, int sourceHeight = 0) const = 0;

    static QString formatBitrate(qint64 bitsPerSecond)
    {
        if (bitsPerSecond <= 0)
            return QStringLiteral("unknown");
        const double mbps = static_cast<double>(bitsPerSecond) / 1'000'000.0;
        if (mbps >= 10.0)
            return QString::number(std::llround(mbps)) + QStringLiteral(" Mbps");
        if (mbps >= 1.0)
            return QString::number(mbps, 'f', 1) + QStringLiteral(" Mbps");
        return QString::number(std::llround(mbps * 1000.0)) + QStringLiteral(" kbps");
    }

    static QString describeResolution(int height)
    {
        if (height <= 0)
            return QStringLiteral("Source");
        if (height >= 2160)
            return QStringLiteral("4K");
        return QString::number(height) + QLatin1Char('p');
    }

    static std::vector<Rung> defaultLadder(qint64 sourceBitrate, int sourceHeight = 0)
    {
        static constexpr struct {
            qint64 bitrate;
            int height;
        } kStandardRungs[] = {
            { 120'000'000, 2160 },
            { 80'000'000, 2160 },
            { 60'000'000, 1080 },
            { 40'000'000, 1080 },
            { 20'000'000, 1080 },
            { 15'000'000, 1080 },
            { 10'000'000, 720 },
            { 8'000'000, 720 },
            { 6'000'000, 720 },
            { 4'000'000, 480 },
            { 3'000'000, 480 },
            { 2'000'000, 480 },
            { 1'000'000, 360 },
        };

        std::vector<Rung> options;
        options.reserve(std::size(kStandardRungs));
        for (const auto& rung : kStandardRungs) {
            if (sourceBitrate > 0 && rung.bitrate >= sourceBitrate)
                continue;
            if (sourceHeight > 0 && rung.height > sourceHeight)
                continue;
            options.push_back({
                describeResolution(rung.height) + QStringLiteral(" · ") + formatBitrate(rung.bitrate),
                rung.bitrate,
                rung.height,
            });
        }
        return options;
    }
};

} // namespace JellyfinNative
