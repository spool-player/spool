#include "api/JellyfinJsAdapter.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

JELLYFIN_TEST_MAIN("jellyfin-js-adapter")
{
    QCoreApplication app(argc, argv);
    using namespace JellyfinNative;

    QTemporaryDir directory;
    require(directory.isValid(), "isolated persistent state directory created");
    qputenv("JELLYFIN_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());

    DatabaseManager database;
    require(database.initialize(directory.filePath("cache.sqlite")), "database initialized");

    ProviderRegistry registry;
    registry.registerModule(
        QStringLiteral("spool.jellyfin"), QStringLiteral("qrc:/providers/spool.jellyfin/logic/provider.mjs"));
    QCoro::waitFor(registry.restoreSources(&database));

    const QString secret = QStringLiteral("test-access-token-12345");
    const QString server = QStringLiteral("https://media.test.local:8096");
    const QString sourceId = QCoro::waitFor(registry.configureSource(QStringLiteral("spool.jellyfin"),
        QStringLiteral("test-user-id"), QStringLiteral("library"), QStringLiteral("Test Server"),
        { { QStringLiteral("server"), server }, { QStringLiteral("userId"), QStringLiteral("test-user-id") },
            { QStringLiteral("token"), secret }, { QStringLiteral("deviceId"), QStringLiteral("test-device") },
            { QStringLiteral("deviceName"), QStringLiteral("Spool Test") },
            { QStringLiteral("clientVersion"), QStringLiteral("0.3.0") } },
        { QUrl(server) }));

    require(!sourceId.isEmpty(), "JS source configured in registry");

    JellyfinJsAdapter adapter(&registry);
    require(!adapter.signedIn(), "adapter initially signed out");
    require(adapter.libraryScopeKey().isEmpty(), "libraryScopeKey empty when signed out");

    adapter.configure(sourceId, server, secret);
    adapter.setDeviceId(QStringLiteral("test-device"));
    require(adapter.signedIn(), "adapter signed in after configure");
    require(adapter.libraryScopeKey() == sourceId, "libraryScopeKey matches source ID");

    // Test ArtworkSource
    ArtworkSource::ImageRequest imgReq;
    imgReq.itemId = QStringLiteral("item-42");
    imgReq.imageType = QStringLiteral("Primary");
    imgReq.tag = QStringLiteral("tag-abc");
    imgReq.maxWidth = 400;
    imgReq.quality = 90;
    imgReq.format = QStringLiteral("webp");

    const QString imgUrl = adapter.imageUrl(imgReq);
    require(imgUrl.contains("media.test.local:8096/Items/item-42/Images/Primary"), "imageUrl has correct path");
    require(imgUrl.contains("tag=tag-abc"), "imageUrl includes tag");
    require(imgUrl.contains("maxWidth=400"), "imageUrl includes maxWidth");
    require(imgUrl.contains("quality=90"), "imageUrl includes quality");
    require(imgUrl.contains("format=webp"), "imageUrl includes format");

    // Test PlaybackSource properties
    PlaybackSource *playback = adapter.playback();
    require(playback != nullptr, "playback source exists");
    require(playback->signedIn(), "playback source signed in");
    require(playback->mediaOrigin() == QUrl(server), "mediaOrigin matches server URL");
    require(playback->mediaRequestHeaders().contains("X-Emby-Token: test-access-token-12345"),
        "mediaRequestHeaders contains X-Emby-Token");

    const QString trickplay = adapter.trickplayTileUrl(QStringLiteral("item-42"), 320, 5);
    require(trickplay == "https://media.test.local:8096/Items/item-42/Images/Trickplay/320/5",
        "trickplay tile URL formatted correctly");

    // Test clearing
    adapter.clear();
    require(!adapter.signedIn(), "adapter signed out after clear");
    require(adapter.mediaRequestHeaders().isEmpty(), "headers empty after clear");

    std::cout << "All JellyfinJsAdapter tests passed successfully.\n";
    return 0;
}
