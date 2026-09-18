#include "app/ArtworkService.h"
#include "platform/PlatformSettingsPolicy.h"

#include "RecordingArtworkSource.h"
#include "TestMain.h"

#include <QCoreApplication>
#include <QUrl>
#include <QUrlQuery>

#include <cstdlib>
#include <iostream>
#include <memory>

using namespace JellyfinNative;

namespace {

void require(bool condition, const QString& message)
{
    if (condition)
        return;
    std::cerr << message.toStdString() << '\n';
    std::exit(EXIT_FAILURE);
}

MovieItem sampleItem()
{
    MovieItem item;
    item.id = QStringLiteral("item1");
    item.posterTag = QStringLiteral("tag1");
    item.seriesId = QStringLiteral("series1");
    item.seriesPrimaryImageTag = QStringLiteral("tag2");
    item.thumbTag = QStringLiteral("tag3");
    item.backdropTag = QStringLiteral("tag4");
    item.bannerTag = QStringLiteral("tag5");
    item.logoTag = QStringLiteral("tag6");
    return item;
}

QString parameter(const QString& url, const QString& name)
{
    return QUrlQuery(QUrl(url).query()).queryItemValue(name);
}

JellyfinNative::Testing::RecordingArtworkSource g_source;

std::unique_ptr<ArtworkService> service()
{
    auto artwork = std::make_unique<ArtworkService>(QString(), 0, 1024, 1, nullptr);
    artwork->setSource(&g_source);
    return artwork;
}

// The qualities the app asked for before the setting existed. Jellyfin caches
// what it encodes under the exact request, so a client that drifts off these
// numbers stops sharing that cache with every other client.
void defaultsReproduceTheShippedQualities()
{
    auto artwork = service();
    artwork->setArtworkEncoding(QStringLiteral("webp"), 75, 82);
    const MovieItem item = sampleItem();
    const QVariant value = QVariant::fromValue(item);

    const struct {
        const char *kind;
        const char *quality;
    } expected[] = { { "poster", "75" }, { "seriesPoster", "80" }, { "square", "80" }, { "landscape", "68" },
        { "backdrop", "82" }, { "banner", "86" }, { "thumb", "82" } };

    for (const auto& kind : expected) {
        const QString url = artwork->url(value, QString::fromLatin1(kind.kind));
        require(!url.isEmpty(), QStringLiteral("no url for %1").arg(QString::fromLatin1(kind.kind)));
        require(parameter(url, QStringLiteral("quality")) == QString::fromLatin1(kind.quality),
            QStringLiteral("%1 asked for quality %2, expected %3")
                .arg(QString::fromLatin1(kind.kind), parameter(url, QStringLiteral("quality")),
                    QString::fromLatin1(kind.quality)));
        require(parameter(url, QStringLiteral("format")) == QStringLiteral("webp"),
            QStringLiteral("%1 was not requested as webp").arg(QString::fromLatin1(kind.kind)));
    }
}

// One slider moves every kind together and keeps the spacing between them.
void qualityOffsetsSurviveADifferentBase()
{
    auto artwork = service();
    artwork->setArtworkEncoding(QStringLiteral("jpeg"), 75, 82);
    const QVariant value = QVariant::fromValue(sampleItem());

    require(parameter(artwork->url(value, QStringLiteral("poster")), QStringLiteral("quality")) == QStringLiteral("82"),
        QStringLiteral("jpeg poster did not use the jpeg base quality"));
    require(parameter(artwork->url(value, QStringLiteral("seriesPoster")), QStringLiteral("quality"))
            == QStringLiteral("87"),
        QStringLiteral("jpeg series poster lost its offset above the base"));
    require(
        parameter(artwork->url(value, QStringLiteral("landscape")), QStringLiteral("quality")) == QStringLiteral("75"),
        QStringLiteral("jpeg landscape lost its offset below the base"));
    require(
        parameter(artwork->url(value, QStringLiteral("poster")), QStringLiteral("format")) == QStringLiteral("jpeg"),
        QStringLiteral("jpeg was not requested"));
}

// Logos carry transparency, so they stay PNG whatever the user picked, and a
// lossless image gains nothing from a quality number that would only split
// clients across cache entries.
void logosStayLosslessAndPinned()
{
    auto artwork = service();
    const QVariant value = QVariant::fromValue(sampleItem());
    for (const int base : { 40, 75, 100 }) {
        artwork->setArtworkEncoding(QStringLiteral("jpeg"), base, base);
        const QString url = artwork->url(value, QStringLiteral("logo"));
        require(parameter(url, QStringLiteral("format")) == QStringLiteral("png"),
            QStringLiteral("logo was not requested as png"));
        require(parameter(url, QStringLiteral("quality")) == QStringLiteral("90"),
            QStringLiteral("logo quality moved with the slider"));
    }
}

// An offset must never push a request off the end of the scale.
void qualityStaysInRange()
{
    auto artwork = service();
    const QVariant value = QVariant::fromValue(sampleItem());

    artwork->setArtworkEncoding(QStringLiteral("webp"), 100, 100);
    require(
        parameter(artwork->url(value, QStringLiteral("banner")), QStringLiteral("quality")) == QStringLiteral("100"),
        QStringLiteral("a positive offset escaped the top of the range"));

    artwork->setArtworkEncoding(QStringLiteral("webp"), 500, 500);
    require(
        parameter(artwork->url(value, QStringLiteral("poster")), QStringLiteral("quality")) == QStringLiteral("100"),
        QStringLiteral("an out-of-range base was not clamped"));
}

// A value that is neither codec must not reach the server as one.
void unknownFormatFallsBackToThePlatformDefault()
{
    auto artwork = service();
    artwork->setArtworkEncoding(QStringLiteral("avif"), 75, 82);
    const QString url = artwork->url(QVariant::fromValue(sampleItem()), QStringLiteral("poster"));
    require(parameter(url, QStringLiteral("format")) == QString::fromLatin1(platformDefaultArtworkFormat()),
        QStringLiteral("an unsupported format was passed through to the server"));
}

} // namespace

JELLYFIN_TEST_MAIN("artwork-url")
{
    QCoreApplication app(argc, argv);
    defaultsReproduceTheShippedQualities();
    qualityOffsetsSurviveADifferentBase();
    logosStayLosslessAndPinned();
    qualityStaysInRange();
    unknownFormatFallsBackToThePlatformDefault();
    return EXIT_SUCCESS;
}
