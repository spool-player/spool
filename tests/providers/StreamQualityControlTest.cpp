#include "provider/StreamQualityControl.h"

#include "TestMain.h"

#include <QDebug>
#include <QString>

#include <cstdlib>

using Spool::StreamQualityControl;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

} // namespace

SPOOL_TEST_MAIN("stream-quality-control")
{
    // Format bitrate
    require(StreamQualityControl::formatBitrate(40'000'000) == QStringLiteral("40 Mbps"),
        "formatBitrate should format tens of megabits cleanly");
    require(StreamQualityControl::formatBitrate(3'000'000) == QStringLiteral("3.0 Mbps"),
        "formatBitrate should format single-digit megabits with one decimal place");
    require(StreamQualityControl::formatBitrate(720'000) == QStringLiteral("720 kbps"),
        "formatBitrate should format kilobits cleanly");
    require(StreamQualityControl::formatBitrate(0) == QStringLiteral("unknown"),
        "formatBitrate should report unknown for zero");
    require(StreamQualityControl::formatBitrate(-1) == QStringLiteral("unknown"),
        "formatBitrate should report unknown for negative values");

    // Describe resolution
    require(StreamQualityControl::describeResolution(0) == QStringLiteral("Source"),
        "describeResolution should report Source for 0");
    require(StreamQualityControl::describeResolution(2160) == QStringLiteral("4K"),
        "describeResolution should report 4K for 2160");
    require(StreamQualityControl::describeResolution(1080) == QStringLiteral("1080p"),
        "describeResolution should report 1080p for 1080");
    require(StreamQualityControl::describeResolution(720) == QStringLiteral("720p"),
        "describeResolution should report 720p for 720");
    require(StreamQualityControl::describeResolution(480) == QStringLiteral("480p"),
        "describeResolution should report 480p for 480");

    // Default ladder without constraints
    const auto fullLadder = StreamQualityControl::defaultLadder(0, 0);
    require(!fullLadder.empty(), "defaultLadder should provide options when unconstrained");
    require(fullLadder.front().height == 2160, "highest rung in full ladder should be 4K");
    for (const auto& rung : fullLadder) {
        require(!rung.label.isEmpty(), "each rung should have a non-empty label");
        require(rung.bitrate > 0, "each rung should have a positive bitrate");
        require(rung.height > 0, "each rung should have a positive height");
    }

    // Default ladder with bitrate constraint
    const auto constrainedBitrate = StreamQualityControl::defaultLadder(12'000'000, 0);
    require(!constrainedBitrate.empty(), "constrained ladder should have rungs below source bitrate");
    for (const auto& rung : constrainedBitrate) {
        require(rung.bitrate < 12'000'000, "all rungs must have bitrate below source bitrate");
    }

    // Default ladder with height constraint
    const auto constrainedHeight = StreamQualityControl::defaultLadder(0, 1080);
    require(!constrainedHeight.empty(), "constrained ladder should have rungs at or below source height");
    for (const auto& rung : constrainedHeight) {
        require(rung.height <= 1080, "all rungs must have height at or below source height");
    }

    // Known resolution is not capped by the stored average bitrate.
    const auto constrainedBoth = StreamQualityControl::defaultLadder(10'000'000, 720);
    require(!constrainedBoth.empty(), "known-resolution ladder should retain useful resolution ceilings");
    for (const auto& rung : constrainedBoth)
        require(rung.height <= 720, "known-resolution ladder must not offer upscaling");
    const auto compressed4K = StreamQualityControl::defaultLadder(5'935'000, 2160);
    require(compressed4K.front().height == 2160, "compressed 4K must retain 4K quality choices");
    bool has1080 = false;
    bool has720 = false;
    for (const auto& rung : compressed4K) {
        has1080 |= rung.height == 1080;
        has720 |= rung.height == 720;
    }
    require(has1080 && has720, "compressed 4K must not lose HD choices because its average bitrate is low");

    return EXIT_SUCCESS;
}
