#include "provider/ProviderRegistry.h"
#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "platform/CredentialStore.h"
#include "provider/Provider.h"
#include "provider/ProviderUiContext.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QLocale>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>

using namespace Spool;

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void waitUntil(const std::function<bool()>& condition, const char *message)
{
    QElapsedTimer timeout;
    timeout.start();
    while (!condition() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}

const ProviderAccount *find(const ProviderRegistry& registry, const QString& label)
{
    for (const ProviderAccount& account : registry.accountList()) {
        if (account.label == label)
            return &account;
    }
    return nullptr;
}

template <typename T> QString failure(QCoro::Task<T> task)
{
    try {
        QCoro::waitFor(std::move(task));
    } catch (const std::exception& error) {
        return QString::fromLatin1(error.what());
    }
    return {};
}

// Signs in through the fixture's login context the way its QML would.
QString signIn(ProviderRegistry& registry, const QString& key, const QString& group, const QString& origin = {})
{
    auto *login = qobject_cast<ProviderUiContext *>(registry.beginSetup(QStringLiteral("fixture.test")));
    require(login && login->role() == QStringLiteral("login")
            && login->component().toString().endsWith(QStringLiteral("ui/Login.qml")),
        "a provider with a login screen starts setup with a login context");
    if (!origin.isEmpty())
        QCoro::waitFor(registry.allowSetupOrigin(login->sourceId(), QUrl(origin)));
    QString added;
    const auto connection
        = QObject::connect(&registry, &ProviderRegistry::accountAdded, [&added](const QString& id) { added = id; });
    const QString label = key.front().toUpper() + key.mid(1);
    login->complete(
        { { QStringLiteral("account"), key }, { QStringLiteral("label"), label }, { QStringLiteral("group"), group },
            { QStringLiteral("configuration"),
                QVariantMap { { QStringLiteral("label"), label }, { QStringLiteral("token"), key + "-secret" } } } });
    waitUntil([&] { return !added.isEmpty(); }, "the authenticated account is committed");
    QObject::disconnect(connection);
    require(!added.isEmpty(), "completing the login adds the account");
    waitUntil([&] { return registry.sourceRunning(added); }, "a new account starts at once");
    return added;
}

} // namespace

SPOOL_TEST_MAIN("provider-registry")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QString credentials = directory.filePath(QStringLiteral("credentials"));
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", credentials.toUtf8());
    const QString installs = directory.filePath(QStringLiteral("providers"));
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "database opens");
    QCoro::waitFor(database.schemaVersionAsync());

    // Two users of one server, as the native Jellyfin client stored them.
    require(CredentialStore::save(QStringLiteral("legacy-a"), QStringLiteral("token-a"))
            && CredentialStore::save(QStringLiteral("legacy-b"), QStringLiteral("token-b")),
        "legacy tokens stored");
    {
        QSqlDatabase seed = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy"));
        seed.setDatabaseName(directory.filePath(QStringLiteral("state.sqlite")));
        require(seed.open(), "durable state opens");
        QSqlQuery query(seed);
        require(query.exec(QStringLiteral("INSERT INTO profiles VALUES "
                                          "('legacy-a', 'server', 'Home', 'https://jf.invalid:8920/jf', 'ua', 'Ann', "
                                          "'', 5, 1, 0), ('legacy-b', 'server', 'Home', 'https://jf.invalid:8920/jf', "
                                          "'ub', 'Ben', '', 9, 1, 0)")),
            "legacy profiles seeded");
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy"));

    require(ProviderPackage::install(ProviderFixture::package(), installs).has_value(), "fixture installs");
    QString alice;
    QString carol;
    {
        ProviderRegistry registry(&database);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        const ProviderModule *jellyfin = registry.module(QStringLiteral("spool.jellyfin"));
        require(jellyfin && jellyfin->bundled && jellyfin->root.scheme() == QStringLiteral("qrc"),
            "the pinned Jellyfin package is bundled as a resource");
        const ProviderModule *fixture = registry.module(QStringLiteral("fixture.test"));
        require(fixture && !fixture->bundled && fixture->root.isLocalFile(), "installed packages are found on disk");

        QStringList problems;
        QObject::connect(
            &registry, &ProviderRegistry::problem, [&problems](const QString& message) { problems.append(message); });
        QHash<QString, QPointer<Provider>> providers;
        QObject::connect(&registry, &ProviderRegistry::sourceStarted,
            [&providers](Provider *provider) { providers.insert(provider->id(), provider); });

        QCoro::waitFor(registry.restore());
        require(registry.restored(), "restore completes");

        // One-time carry-over of the native client's sign-ins: one account per
        // user, the most recently used on each server in use.
        const ProviderAccount *ann = find(registry, QStringLiteral("Ann"));
        const ProviderAccount *ben = find(registry, QStringLiteral("Ben"));
        require(registry.accountList().size() == 2 && ann && ben, "legacy sign-ins become Jellyfin accounts");
        require(ben->module == QStringLiteral("spool.jellyfin") && ben->group == QStringLiteral("server")
                && ben->detail == QStringLiteral("Home"),
            "a migrated account keeps its server as its group");
        require(ben->enabled && !ann->enabled, "only the last user of a server is in use");
        require(ben->configuration.value(QStringLiteral("token")).toString() == QStringLiteral("token-b"),
            "the token moves into the account's configuration");
        require(ben->origins == QList<QUrl> { QUrl(QStringLiteral("https://jf.invalid:8920")) },
            "the server's origin, and nothing else, is allowed");
        for (const QString& id : { ann->id, ben->id }) {
            registry.setAccountEnabled(id, false);
            registry.removeAccount(id);
        }
        require(registry.accountList().empty(), "migrated accounts can be removed");

        bool denied = false;
        auto *probe = qobject_cast<ProviderUiContext *>(registry.beginSetup(QStringLiteral("fixture.test")));
        try {
            QCoro::waitFor(registry.allowSetupOrigin(probe->sourceId(), QUrl(QStringLiteral("file:///etc/passwd"))));
        } catch (const std::exception&) {
            denied = true;
        }
        require(denied, "setup may only reach HTTP(S) origins");
        probe->close();

        alice = signIn(registry, QStringLiteral("alice"), QStringLiteral("server-1"),
            QStringLiteral("http://127.0.0.1:9/some/path"));
        require(find(registry, QStringLiteral("Alice"))->origins
                == QList<QUrl> { QUrl(QStringLiteral("http://127.0.0.1:9")) },
            "the account keeps the origin the viewer allowed during setup");
        require(QCoro::waitFor(registry.callSource(alice, QStringLiteral("configuration")))
                    .value(QStringLiteral("configuration"))
                    .toMap()
                    .value(QStringLiteral("token"))
                == QStringLiteral("alice-secret"),
            "the account runs with the configuration its login completed with");

        // Accounts on one server are alternatives; different servers show together.
        const QString bob = signIn(registry, QStringLiteral("bob"), QStringLiteral("server-1"));
        require(!find(registry, QStringLiteral("Alice"))->enabled && !registry.sourceRunning(alice),
            "using another user of a server sets the first aside");
        carol = signIn(registry, QStringLiteral("carol"), QStringLiteral("server-2"));
        require(registry.sourceRunning(bob) && registry.sourceRunning(carol), "different servers run side by side");
        require(signIn(registry, QStringLiteral("alice"), QStringLiteral("server-1")) == alice,
            "signing in again updates the existing account");
        require(registry.accountList().size() == 3 && !registry.sourceRunning(bob), "and makes it the one in use");

        require(failure(registry.callSource(carol, QStringLiteral("throws"))) == QStringLiteral("provider_error"),
            "provider error text never reaches the app");
        require(failure(registry.callSource(carol, QStringLiteral("expired"))) == QStringLiteral("http_401"),
            "stable error codes pass through");
        bool needsSignIn = false;
        for (const QVariant& row : registry.accounts())
            needsSignIn |= row.toMap().value(QStringLiteral("id")) == carol
                && row.toMap().value(QStringLiteral("needsSignIn")).toBool();
        require(needsSignIn && problems.contains(QStringLiteral("Sign in to Carol again")),
            "a rejected token asks for a new sign-in");
        require(failure(registry.callSource(QStringLiteral("nobody"), QStringLiteral("state")))
                == QStringLiteral("source_unavailable"),
            "unknown accounts are refused");

        QString changed;
        waitUntil([&] { return bool(providers.value(carol)); }, "the running account is announced");
        QObject::connect(
            providers.value(carol), &Provider::contentChanged, [&changed](const QString& itemId) { changed = itemId; });
        QCoro::waitFor(registry.callSource(
            carol, QStringLiteral("announce"), { { QStringLiteral("itemId"), QStringLiteral("x") } }));
        waitUntil([&] { return changed == QStringLiteral("x"); }, "events a source emits reach its provider");
    }

    // Everything above survives a restart; tokens only in the credential store.
    for (QDirIterator it(directory.path(), QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const QString path = it.next();
        if (path.startsWith(credentials))
            continue;
        QFile file(path);
        require(file.open(QIODevice::ReadOnly) && !file.readAll().contains("alice-secret"),
            "no token is written outside the credential store");
    }
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(installs);
    registry.loadModules();
    QCoro::waitFor(registry.restore());
    require(registry.accountList().size() == 3, "accounts are restored");
    const ProviderAccount *bob = find(registry, QStringLiteral("Bob"));
    require(
        bob && !bob->enabled && find(registry, QStringLiteral("Alice"))->enabled, "which one is in use is restored");
    // Started means described and handed to the app, not merely launched.
    const auto started = [&](const QString& id) {
        for (const QVariant& row : registry.accounts()) {
            if (row.toMap().value(QStringLiteral("id")) == id)
                return row.toMap().value(QStringLiteral("running")).toBool();
        }
        return false;
    };
    waitUntil([&] { return started(alice) && started(carol); }, "enabled accounts start on restore");
    require(QCoro::waitFor(registry.callSource(alice, QStringLiteral("configuration")))
                .value(QStringLiteral("configuration"))
                .toMap()
                .value(QStringLiteral("token"))
            == QStringLiteral("alice-secret"),
        "configuration comes back from the credential store");

    // A newer package replaces the module in place and restarts its accounts.
    int restarted = 0;
    QObject::connect(&registry, &ProviderRegistry::sourceStarted, [&restarted](Provider *) { ++restarted; });
    QCoro::waitFor(registry.install(ProviderFixture::package(QStringLiteral("fixture.test"), QStringLiteral("1.1.0"))));
    require(registry.module(QStringLiteral("fixture.test"))->manifest.version == QStringLiteral("1.1.0"),
        "the newer version is loaded");
    waitUntil([&] { return restarted >= 2 && started(alice) && started(carol); },
        "running accounts restart on the new version");

    QCoro::waitFor(registry.uninstall(QStringLiteral("spool.jellyfin")));
    require(registry.module(QStringLiteral("spool.jellyfin")), "a bundled provider cannot be removed");
    QCoro::waitFor(registry.uninstall(QStringLiteral("fixture.test")));
    require(!registry.module(QStringLiteral("fixture.test")) && registry.accountList().empty()
            && !QDir(installs).exists(QStringLiteral("fixture.test")),
        "removing a provider removes its files and accounts");
    // Negotiation is account-local, exact-versioned, and complete before publication.
    auto extensionsPackage = ProviderFixture::package(QStringLiteral("fixture.test"), QStringLiteral("2.0.0"));
    auto manifest = QJsonDocument::fromJson(extensionsPackage.files.value("manifest.json")).object();
    manifest.insert("extensions",
        QJsonObject { { "spool.speed-test", 1 }, { "spool.suggestions", 1 }, { "future.feature", 7 },
            { "spool.item-actions", 2 }, { "spool.origin-grants", 1 }, { "spool.lan-probe", 1 },
            { "spool.playback-preferences", 1 }, { "spool.settings-storage", 1 } });
    extensionsPackage.files["manifest.json"] = QJsonDocument(manifest).toJson();
    extensionsPackage.manifest = *ProviderManifest::parse(extensionsPackage.files.value("manifest.json"));
    extensionsPackage.files["logic/provider.mjs"] = R"JS(
export function createSource(config, sourceHost) {
    let calls = 0;
    let infoCalls = 0, writes = 0, preferenceWrites = 0;
    let maximum = 64, conditional = false, document = {found:false};
    let preferences = {audioLanguage:'en', subtitleMode:'None'};
    const offers = config.label === 'Alice' ? {'spool.speed-test': 1, 'spool.suggestions': 1, 'spool.origin-grants': 1,
        'spool.playback-preferences':1, 'spool.settings-storage':1}
        : {'spool.speed-test': 2, 'spool.suggestions': 1};
    return {
        describe() { return sourceHost.delay(30).then(function() {
            return {extensions: config.label === 'Invalid' ? {'spool.speed-test': '1'} : offers};
        }); },
        state() { return {calls, extensions: sourceHost.extensions}; },
        offers(args) { sourceHost.emit('extensionsChanged', {extensions: args.extensions}); return {}; },
        fetch(args, host) { return host.http(args.url); },
        dataFixture(args) {
            if (args.maximum !== undefined) maximum = args.maximum;
            if (args.conditional !== undefined) conditional = args.conditional;
            if (args.document !== undefined) document = args.document;
            return {infoCalls, writes, preferenceWrites};
        },
        dataInfo() { ++infoCalls; return {maxBytes:maximum, conditionalWrites:conditional}; },
        dataRead() { return document; },
        dataWrite(args) {
            ++writes;
            if (Object.prototype.hasOwnProperty.call(args, 'expectedRevision')) {
                const revision = document.found ? document.revision : null;
                if (args.expectedRevision !== revision) throw new Error('conflict');
            }
            document = {found:true,value:args.value};
            if (conditional) { document.revision = 'r' + writes; return {revision:document.revision}; }
            return {};
        },
        dataDelete() { ++writes; document = {found:false}; return {}; },
        preferencesRead() { return {values:preferences,writable:['audioLanguage']}; },
        preferencesWrite(args) { ++preferenceWrites; preferences.audioLanguage = args.values.audioLanguage; return {}; },
        speedTest() { ++calls; return {bitrate: 2000000, parallelRequests: 1}; },
        suggestions(args, host) {
            ++calls;
            host.emit('suggestionsStarted', {});
            return host.delay(args.delay || 0).then(function() {
                return {items: [{id:'suggestion', title:'Suggested', type:'Movie'}], cursor:null, exhausted:true};
            });
        },
        activate() { ++calls; return {}; },
        signOut() { return {}; }
    };
}
)JS";
    QCoro::waitFor(registry.install(std::move(extensionsPackage)));
    SourceHub hub(&registry);
    hub.setPlaybackActive(true);
    QHash<QString, QPointer<Provider>> extendedProviders;
    QObject::connect(&registry, &ProviderRegistry::sourceStarted, &app, [&](Provider *provider) {
        extendedProviders.insert(provider->id(), provider);
        require(registry.extensionVersion(provider->id(), "spool.suggestions") == 1,
            "effective offers exist before a source is published");
    });
    auto *draft = qobject_cast<ProviderUiContext *>(registry.beginSetup("fixture.test"));
    require(draft->extensions().value("spool.speed-test").toInt() == 1
            && !draft->extensions().contains("future.feature")
            && draft->missingHostExtensions() == QStringList { "future.feature", "spool.item-actions" },
        "login receives only host-supported declared versions and separate missing-host metadata");
    require(failure(registry.callExtension(draft->sourceId(), "spool.suggestions", "suggestions"))
            == "unsupported_extension",
        "drafts cannot call account extensions");
    draft->close();
    const QString extendedAlice = signIn(registry, "alice", "extension-a");
    const QString extendedCarol = signIn(registry, "carol", "extension-c");
    require(registry.extensionVersion(extendedAlice, "spool.speed-test") == 1
            && registry.extensionVersion(extendedCarol, "spool.speed-test") == 0,
        "accounts of one module negotiate independently at the exact wire major");
    require(extendedProviders[extendedAlice]->capabilities().testFlag(Provider::SpeedTest)
            && !extendedProviders[extendedCarol]->capabilities().testFlag(Provider::SpeedTest)
            && hub.capabilities().testFlag(Provider::SpeedTest),
        "negotiated speed testing augments account and aggregate capabilities");
    auto *accountContext = qobject_cast<ProviderUiContext *>(registry.openPicker(extendedCarol, {}));
    require(accountContext && !accountContext->extensions().contains("spool.speed-test")
            && accountContext->extensions().value("spool.suggestions").toInt() == 1,
        "account UI receives effective offers rather than the module host map");
    require(failure(registry.callSource(extendedCarol, "speedTest")) == "unsupported_extension"
            && failure(registry.callExtension(extendedAlice, "spool.suggestions", "speedTest"))
                == "unsupported_extension"
            && failure(registry.callSource(extendedAlice, "activate")) == "action_unavailable",
        "unsupported, mismatched and private operations fail before provider execution");
    require(QCoro::waitFor(registry.callSource(extendedCarol, "state")).value("calls").toInt() == 0
            && QCoro::waitFor(registry.callSource(extendedAlice, "state")).value("calls").toInt() == 0,
        "rejected calls execute no provider work");
    const auto page
        = QCoro::waitFor(registry.callExtensionMediaPage(extendedAlice, "spool.suggestions", "suggestions", {}, 10));
    require(page.items.size() == 1 && page.items.front().id == "suggestion" && page.exhausted,
        "negotiated media operations use the typed worker decoder");

    const QString documentKey = QStringLiteral("278fca80-aaf9-4d32-8458-388836790234");
    require(QCoro::waitFor(registry.callSource(extendedAlice, "dataFixture")).value("infoCalls").toInt() == 0,
        "storage discovery is not an eager startup request");
    require(failure(registry.callSource(extendedAlice, "dataWrite", { { "key", "../settings" }, { "value", 1 } }))
                == "invalid_data"
            && QCoro::waitFor(registry.callSource(extendedAlice, "dataFixture")).value("infoCalls").toInt() == 0,
        "invalid document keys fail before optional backend discovery");
    const QVariant jsonNull = QVariant::fromValue(nullptr);
    require(failure(registry.callSource(extendedAlice, "dataWrite",
                { { "key", documentKey }, { "value", jsonNull }, { "expectedRevision", jsonNull } }))
                == "unsupported_condition"
            && QCoro::waitFor(registry.callSource(extendedAlice, "dataFixture")).value("writes").toInt() == 0,
        "the QML callSource route rejects weak-store conditions before dispatching a mutation");
    QCoro::waitFor(registry.callSource(extendedAlice, "dataWrite", { { "key", documentKey }, { "value", jsonNull } }));
    const auto storedNull = QCoro::waitFor(registry.callSource(extendedAlice, "dataRead", { { "key", documentKey } }));
    require(storedNull.value("found").toBool() && storedNull.contains("value")
            && storedNull.value("value").metaType().id() == QMetaType::Nullptr,
        "actual worker transport preserves a stored null instead of returning absence");
    require(failure(registry.callSource(
                extendedAlice, "dataWrite", { { "key", documentKey }, { "value", QString(64, QLatin1Char('x')) } }))
                == "data_too_large"
            && QCoro::waitFor(registry.callSource(extendedAlice, "dataFixture")).value("writes").toInt() == 1
            && QCoro::waitFor(registry.callSource(extendedAlice, "dataFixture")).value("infoCalls").toInt() == 1,
        "cached provider limits reject an oversized write without a mutation or repeated discovery");
    QCoro::waitFor(registry.callSource(
        extendedAlice, "dataFixture", { { "document", QVariantMap { { "found", false }, { "value", jsonNull } } } }));
    require(failure(registry.callSource(extendedAlice, "dataRead", { { "key", documentKey } }))
            == "invalid_extension_result",
        "malformed storage reads cannot masquerade as absent values");
    auto enabledOffers = registry.extensions(extendedAlice);
    auto withoutStorage = enabledOffers;
    withoutStorage.remove("spool.settings-storage");
    QCoro::waitFor(registry.callSource(extendedAlice, "offers", { { "extensions", withoutStorage } }));
    QCoro::waitFor(registry.callSource(extendedAlice, "dataFixture",
        { { "maximum", 32 }, { "conditional", true }, { "document", QVariantMap { { "found", false } } } }));
    QCoro::waitFor(registry.callSource(extendedAlice, "offers", { { "extensions", enabledOffers } }));
    require(QCoro::waitFor(registry.callSource(extendedAlice, "dataInfo")).value("maxBytes").toInt() == 32
            && QCoro::waitFor(registry.callSource(extendedAlice, "dataFixture")).value("infoCalls").toInt() == 2,
        "extension loss invalidates the generation-owned storage metadata cache");
    QCoro::waitFor(registry.callSource(
        extendedAlice, "dataWrite", { { "key", documentKey }, { "value", 1 }, { "expectedRevision", jsonNull } }));
    require(failure(registry.callSource(extendedAlice, "dataWrite",
                { { "key", documentKey }, { "value", 2 }, { "expectedRevision", jsonNull } }))
                == "conflict"
            && QCoro::waitFor(registry.callSource(extendedAlice, "dataRead", { { "key", documentKey } }))
                    .value("value")
                    .toInt()
                == 1,
        "create-if-absent reaches a real conditional backend and conflicts preserve its existing document");
    require(QCoro::waitFor(registry.callSource(extendedAlice, "preferencesRead"))
                .value("values")
                .toMap()
                .value("audioLanguage")
            == "eng",
        "actual provider preferences normalize two-letter language codes");
    require(failure(registry.callSource(
                extendedAlice, "preferencesWrite", { { "values", QVariantMap { { "subtitleMode", "Always" } } } }))
                == "preference_read_only"
            && QCoro::waitFor(registry.callSource(extendedAlice, "dataFixture")).value("preferenceWrites").toInt() == 0,
        "advertised read-only fields are enforced before invoking the provider writer");
    QCoro::waitFor(registry.callSource(
        extendedAlice, "preferencesWrite", { { "values", QVariantMap { { "audioLanguage", "fra" } } } }));
    require(QCoro::waitFor(registry.callSource(extendedAlice, "preferencesRead"))
                .value("values")
                .toMap()
                .value("audioLanguage")
            == QLocale::languageToCode(QLocale::French, QLocale::ISO639Part2),
        "a supported writable preference survives a worker read/write/read cycle");
    QCoro::waitFor(registry.callSource(extendedAlice, "offers", { { "extensions", withoutStorage } }));
    QCoro::waitFor(
        registry.callSource(extendedAlice, "dataFixture", { { "maximum", 65536 }, { "conditional", false } }));
    QCoro::waitFor(registry.callSource(extendedAlice, "offers", { { "extensions", enabledOffers } }));
    QVariantList wideDocument;
    wideDocument.reserve(20000);
    for (int index = 0; index < 20000; ++index)
        wideDocument.append(index == 10001 ? 1e100 : 0.0);
    QCoro::waitFor(
        registry.callSource(extendedAlice, "dataWrite", { { "key", documentKey }, { "value", wideDocument } }));
    require(QCoro::waitFor(registry.callSource(extendedAlice, "dataRead", { { "key", documentKey } }))
                .value("value")
                .toList()
            == wideDocument,
        "bounded JSON documents round-trip arrays beyond catalogue row limits and all finite JSON numbers");
    const QVariantMap namedProperties { { "__proto__", QVariantMap { { "retained", true } } },
        { "constructor", "ordinary data" }, { "toString", QVariant::fromValue(nullptr) } };
    QCoro::waitFor(
        registry.callSource(extendedAlice, "dataWrite", { { "key", documentKey }, { "value", namedProperties } }));
    require(QCoro::waitFor(registry.callSource(extendedAlice, "dataRead", { { "key", documentKey } }))
                .value("value")
                .toMap()
            == namedProperties,
        "JSON object keys remain data rather than changing JavaScript prototypes");

    QTcpServer local;
    require(local.listen(QHostAddress::LocalHost), "origin fixture listens on loopback only");
    int requests = 0;
    QObject::connect(&local, &QTcpServer::newConnection, &app, [&] {
        while (QTcpSocket *socket = local.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                socket->readAll();
                ++requests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\ngranted");
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    const QUrl peer(QStringLiteral("http://127.0.0.1:%1").arg(local.serverPort()));
    require(!failure(registry.callSource(extendedAlice, "fetch", { { "url", peer.toString() } })).isEmpty()
            && requests == 0,
        "unapproved account origins perform no network work");
    require(failure(registry.requestAccountOrigin(extendedCarol, peer)) == "unsupported_extension",
        "origin permission is gated on this account's negotiated offer");
    for (const QString& bad : { QStringLiteral("https://user:secret@example.invalid"),
             QStringLiteral("https://*.example.invalid"), QStringLiteral("file:///tmp/server"),
             QStringLiteral("https://example.invalid:999999"), QStringLiteral("server.invalid") }) {
        require(failure(registry.requestAccountOrigin(extendedAlice, QUrl(bad, QUrl::StrictMode))) == "origin_denied"
                && registry.networkConsent().isEmpty(),
            "invalid origins are rejected without prompting");
    }
    bool consentSettled = false;
    bool consentApproved = false;
    const auto askOrigin = [&](const QUrl& url, const QString& scope = QString()) {
        consentSettled = consentApproved = false;
        registry.requestAccountOrigin(extendedAlice, url, scope)
            .then([&] { consentSettled = consentApproved = true; },
                [&](const std::exception&) { consentSettled = true; });
        waitUntil([&] { return !registry.networkConsent().isEmpty(); }, "host origin prompt opens");
    };
    askOrigin(peer);
    const QString deniedId = registry.networkConsent().value("id").toString();
    registry.resolveNetworkConsent(deniedId, false);
    waitUntil([&] { return consentSettled; }, "denial settles the request");
    require(!consentApproved && find(registry, "Alice")->origins.isEmpty(),
        "denying an origin changes neither the account nor worker permissions");
    askOrigin(peer, "closing-screen");
    registry.cancelSourceScope(extendedAlice, "closing-screen");
    waitUntil([&] { return consentSettled; }, "closing the requesting scope cancels consent");
    registry.resolveNetworkConsent(deniedId, true);
    require(!consentApproved && registry.networkConsent().isEmpty(), "late approval cannot revive dismissed consent");
    askOrigin(peer, "approved-closing-screen");
    registry.resolveNetworkConsent(registry.networkConsent().value("id").toString(), true);
    registry.cancelSourceScope(extendedAlice, "approved-closing-screen");
    waitUntil([&] { return consentSettled; }, "closing after approval settles the pending grant");
    require(!consentApproved && find(registry, "Alice")->origins.isEmpty()
            && !failure(registry.callSource(extendedAlice, "fetch", { { "url", peer.toString() } })).isEmpty()
            && requests == 0,
        "approval cannot outlive its requesting context or authorize network access after cancellation");
    const QPointer<Provider> beforeGrant = extendedProviders[extendedAlice];
    askOrigin(peer.resolved(QUrl("/resource?private=value#fragment")));
    require(registry.networkConsent().value("origin") == peer.toString()
            && registry.networkConsent().value("account") == "Alice"
            && registry.networkConsent().value("provider") == "Fixture"
            && registry.networkConsent().value("unencrypted").toBool(),
        "the prompt names the provider, account, normalized exact origin and HTTP risk");
    registry.resolveNetworkConsent(registry.networkConsent().value("id").toString(), true);
    waitUntil([&] { return consentSettled; }, "approval updates the worker");
    require(consentApproved && extendedProviders[extendedAlice] == beforeGrant
            && find(registry, "Alice")->origins == QList<QUrl> { peer }
            && QCoro::waitFor(registry.callSource(extendedAlice, "state")).value("calls").toInt() == 1,
        "approval persists the exact origin without restarting or losing source state");
    require(QCoro::waitFor(registry.callSource(extendedAlice, "fetch", { { "url", peer.toString() } })).value("body")
                == "granted"
            && requests == 1,
        "existing operations can immediately use the approved worker origin");
    QCoro::waitFor(registry.requestAccountOrigin(extendedAlice, peer));
    require(registry.networkConsent().isEmpty(), "an already approved exact origin needs no new prompt");

    auto *lanDraft = qobject_cast<ProviderUiContext *>(registry.beginSetup("fixture.test"));
    const QString lanSource = lanDraft->sourceId();
    consentSettled = consentApproved = false;
    registry.allowLanDiscovery(lanSource, "lan")
        .then([&] { consentSettled = consentApproved = true; }, [&](const std::exception&) { consentSettled = true; });
    waitUntil([&] { return !registry.networkConsent().isEmpty(); }, "draft discovery opens host consent");
    require(registry.networkConsent().value("kind") == "lan", "LAN discovery asks for its own bounded permission");
    registry.cancelLanDiscovery(lanSource, "lan");
    waitUntil([&] { return consentSettled; }, "cancelled discovery dismisses its consent");
    require(!consentApproved && !lanDraft->closed(), "discovery cancellation preserves UDP/manual login");
    consentSettled = false;
    registry.allowLanDiscovery(lanSource, "lan")
        .then([&] { consentSettled = consentApproved = true; }, [&](const std::exception&) { consentSettled = true; });
    waitUntil([&] { return !registry.networkConsent().isEmpty(); }, "another user search asks again");
    registry.resolveNetworkConsent(registry.networkConsent().value("id").toString(), true);
    waitUntil([&] { return consentSettled; }, "discovery permission reaches worker");
    require(consentApproved
            && !failure(registry.callSource(lanSource, "fetch", { { "url", peer.toString() } })).isEmpty()
            && requests == 1,
        "LAN consent never grants authenticated HTTP origins");
    require(failure(registry.allowLanDiscovery(extendedAlice, "lan")) == "unsupported_extension",
        "signed-in accounts cannot request subnet discovery");
    lanDraft->close();
    bool pendingStarted = false;
    bool pendingSettled = false;
    bool pendingSucceeded = false;
    QObject::connect(extendedProviders[extendedAlice], &Provider::sourceEvent, &app,
        [&](const QString& type, const QVariantMap&) { pendingStarted |= type == "suggestionsStarted"; });
    registry.callExtension(extendedAlice, "spool.suggestions", "suggestions", { { "delay", 10000 } })
        .then([&](QVariantMap) { pendingSettled = pendingSucceeded = true; },
            [&](const std::exception&) { pendingSettled = true; });
    waitUntil([&] { return pendingStarted; }, "optional operation begins");
    QCoro::waitFor(registry.callSource(extendedAlice, "offers", { { "extensions", QVariantMap {} } }));
    waitUntil([&] { return pendingSettled && registry.extensionVersion(extendedAlice, "spool.speed-test") == 0; },
        "support loss cancels in-flight optional operations");
    require(!pendingSucceeded && !hub.capabilities().testFlag(Provider::SpeedTest)
            && registry.extensionVersion(extendedCarol, "spool.suggestions") == 1,
        "support loss hides aggregate controls without changing another account");
    const auto immutable = QCoro::waitFor(registry.callSource(extendedAlice, "state")).value("extensions").toMap();
    require(immutable.value("spool.speed-test").toInt() == 1,
        "account offers never mutate the source host's compiled declarations");
    QCoro::waitFor(registry.callSource(extendedCarol, "offers",
        { { "extensions",
            QVariantMap { { "spool.suggestions", 1 }, { "spool.speed-test", 1 }, { "future.feature", 7 },
                { "spool.item-actions", 1 } } } }));
    waitUntil([&] { return registry.extensionVersion(extendedCarol, "spool.speed-test") == 1; },
        "new account offers refresh the existing provider");
    require(hub.capabilities().testFlag(Provider::SpeedTest)
            && registry.extensionVersion(extendedCarol, "future.feature") == 0
            && registry.extensionVersion(extendedCarol, "spool.item-actions") == 0,
        "events cannot grant unknown or undeclared wire majors");
    registry.setAccountEnabled(extendedCarol, false);
    require(accountContext->closed() && accountContext->extensions().isEmpty()
            && registry.extensionVersion(extendedCarol, "spool.suggestions") == 0,
        "stopping a source clears account support and closes its UI");
    for (const QVariant& value : registry.accounts()) {
        const auto row = value.toMap();
        if (row.value("id") == extendedCarol)
            require(row.value("connectionState") == "locked", "set-aside accounts are not reported as failed");
    }
    const QString invalid = registry.finishSetup({},
        { { "module", "fixture.test" }, { "account", "invalid" }, { "label", "Invalid" },
            { "configuration", QVariantMap { { "label", "Invalid" } } } });
    const auto stateOf = [&](const QString& id) {
        for (const QVariant& value : registry.accounts()) {
            const auto row = value.toMap();
            if (row.value("id") == id)
                return row.value("connectionState").toString();
        }
        return QString {};
    };
    registry.useAccount(invalid);
    require(stateOf(invalid) == "starting" && !registry.sourceRunning(invalid)
            && failure(registry.callSource(invalid, "state")) == "source_unavailable",
        "unpublished accounts stay starting and cannot perform account calls");
    waitUntil([&] { return stateOf(invalid) == "failed"; }, "malformed offers fail the account startup");
    require(!extendedProviders.contains(invalid) && registry.extensions(invalid).isEmpty(),
        "invalid offers never expose a partially negotiated source");
    QCoro::waitFor(
        registry.callSource(extendedAlice, "offers", { { "extensions", QVariantMap { { "spool.suggestions", 1 } } } }));
    waitUntil([&] { return registry.extensionVersion(extendedAlice, "spool.suggestions") == 1; },
        "the existing source can re-offer a feature");
    pendingStarted = pendingSettled = pendingSucceeded = false;
    registry.callExtension(extendedAlice, "spool.suggestions", "suggestions", { { "delay", 10000 } })
        .then([&](QVariantMap) { pendingSettled = pendingSucceeded = true; },
            [&](const std::exception&) { pendingSettled = true; });
    waitUntil([&] { return pendingStarted; }, "generation-owned optional operation begins");
    registry.restartAccount(extendedAlice);
    require(registry.extensionVersion(extendedAlice, "spool.suggestions") == 1,
        "preparing a replacement keeps the current account available until commit");
    waitUntil([&] { return pendingSettled && registry.sourceRunning(extendedAlice); },
        "restart cancels old operations and negotiates the new generation");
    require(!pendingSucceeded && registry.extensionVersion(extendedAlice, "spool.suggestions") == 1,
        "an old result cannot succeed against the restarted account");
    return 0;
}
