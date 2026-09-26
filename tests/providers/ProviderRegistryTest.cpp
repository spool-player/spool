#include "provider/ProviderRegistry.h"
#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "platform/CredentialStore.h"
#include "provider/Provider.h"
#include "provider/ProviderUiContext.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
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

QString failure(QCoro::Task<QVariantMap> task)
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
    return 0;
}
