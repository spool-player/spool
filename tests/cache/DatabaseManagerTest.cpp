#include "cache/DatabaseManager.h"
#include "platform/CredentialStore.h"

#include "TestMain.h"
#include <QCoroTask>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>

using namespace JellyfinNative;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(1);
}

} // namespace

JELLYFIN_TEST_MAIN("database-manager")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory should be available");
    const QString credentialPath = directory.filePath(QStringLiteral("credentials"));
    qputenv("JELLYFIN_CREDENTIAL_STORE_DIR", credentialPath.toUtf8());
    const QString databasePath = directory.filePath(QStringLiteral("cache.sqlite"));
    const QString statePath = directory.filePath(QStringLiteral("state.sqlite"));

    {
        QSqlDatabase invalid = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("invalid-seed"));
        invalid.setDatabaseName(databasePath);
        require(invalid.open(), "invalid cache seed should open");
        QSqlQuery query(invalid);
        require(query.exec(QStringLiteral("PRAGMA user_version = 999")), "invalid schema should be seeded");
        invalid.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("invalid-seed"));

    DatabaseManager database;
    bool recoveryNotified = false;
    QObject::connect(
        &database, &DatabaseManager::recoveryNotice, [&recoveryNotified](const QString&) { recoveryNotified = true; });
    require(database.initialize(databasePath), "database should rebuild an unsupported cache");
    require(QCoro::waitFor(database.schemaVersionAsync()) == 1, "cache should use the current schema");
    QCoreApplication::processEvents();
    require(recoveryNotified, "cache recovery should produce a user-visible notice");
    require(QDir(directory.path()).entryList({ QStringLiteral("cache.sqlite.corrupt-*") }, QDir::Files).size() == 1,
        "future cache should be preserved as a diagnostic backup");

    database.saveSetting(QStringLiteral("batch/first"), QStringLiteral("one"));
    database.saveSetting(QStringLiteral("batch/second"), QStringLiteral("two"));
    const QVariantMap batch = QCoro::waitFor(database.loadValuesAsync(
        { QStringLiteral("batch/first"), QStringLiteral("batch/second"), QStringLiteral("batch/missing") }));
    require(batch.value(QStringLiteral("batch/first")).toString() == QStringLiteral("one"),
        "batch read should return the first stored value");
    require(batch.value(QStringLiteral("batch/second")).toString() == QStringLiteral("two"),
        "batch read should return the second stored value");
    require(!batch.value(QStringLiteral("batch/missing")).isValid(),
        "batch read should preserve a missing value as invalid");

    // The native Jellyfin client's sign-ins stay readable for the one-time
    // move to provider accounts; the token only ever lives in the credential store.
    const QString legacyToken = QStringLiteral("secret");
    require(CredentialStore::save(QStringLiteral("profile"), legacyToken), "legacy token should be stored");
    {
        QSqlDatabase seed = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy-seed"));
        seed.setDatabaseName(statePath);
        require(seed.open(), "durable state should open for legacy seed");
        QSqlQuery query(seed);
        require(query.exec(QStringLiteral("INSERT INTO profiles VALUES ('profile', 'server', 'Server', "
                                          "'https://example.test', 'user', 'User', '', 2, 1, 0)")),
            "legacy profile should be seeded");
        seed.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy-seed"));
    const auto requireLegacyAccount = [&](DatabaseManager& manager, const char *message) {
        const QVariantList accounts = QCoro::waitFor(manager.loadLegacyAccountsAsync());
        require(accounts.size() == 1, message);
        const QVariantMap account = accounts.front().toMap();
        require(account.value(QStringLiteral("server")).toString() == QStringLiteral("https://example.test")
                && account.value(QStringLiteral("userId")).toString() == QStringLiteral("user")
                && account.value(QStringLiteral("token")).toString() == legacyToken,
            message);
    };
    requireLegacyAccount(database, "legacy sign-in should be readable with its token");
#ifndef Q_OS_WIN
    const QFileInfo credentialInfo(QDir(credentialPath).entryInfoList(QDir::Files).front());
    require((credentialInfo.permissions()
                & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther))
            == 0,
        "credential file should be owner-only");
#endif
    QFile durableState(statePath);
    require(durableState.open(QIODevice::ReadOnly), "durable state should be readable for token inspection");
    require(!durableState.readAll().contains(legacyToken.toUtf8()),
        "SQLite durable state must not contain an access-token copy");
    durableState.close();
    database.saveDeviceId(QStringLiteral("device"));
    const StartupState startup = QCoro::waitFor(
        database.loadStartupStateAsync({ QStringLiteral("batch/first"), QStringLiteral("batch/missing") }));
    require(startup.deviceId == QStringLiteral("device"), "startup state should include the device identifier");
    require(startup.values.value(QStringLiteral("batch/first")).toString() == QStringLiteral("one"),
        "startup state should include requested values");
    require(!startup.values.value(QStringLiteral("batch/missing")).isValid(),
        "startup state should preserve missing values as invalid");

    const QJsonObject homePayload {
        { QStringLiteral("title"), QStringLiteral("Continue Watching") },
        { QStringLiteral("count"), 2 },
    };
    database.saveHomePayload(QStringLiteral("server/user"), 1, homePayload);
    require(QCoro::waitFor(database.loadHomePayloadAsync(QStringLiteral("server/user"), 1)) == homePayload,
        "home payload should load for matching schema");
    require(QCoro::waitFor(database.loadHomePayloadAsync(QStringLiteral("server/user"), 2)).isEmpty(),
        "home payload should not load for a different schema");

    {
        QSqlDatabase tamper = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("payload-tamper"));
        tamper.setDatabaseName(databasePath);
        require(tamper.open(), "cache tamper connection should open");
        QSqlQuery query(tamper);
        require(query.exec(QStringLiteral("UPDATE home_payload SET payload = 'not-json'")),
            "malformed home payload should be seeded");
        tamper.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("payload-tamper"));
    require(QCoro::waitFor(database.loadHomePayloadAsync(QStringLiteral("server/user"), 1)).isEmpty(),
        "malformed home payload should be treated as a cache miss");

    database.saveCacheEntry(QStringLiteral("test"), QStringLiteral("fresh"), QByteArrayLiteral("value"), 5000);
    require(QCoro::waitFor(database.loadCacheEntryAsync(QStringLiteral("test"), QStringLiteral("fresh")))
            == QByteArrayLiteral("value"),
        "fresh cache entry should load");

    database.invalidateCacheNamespace(QStringLiteral("test"));
    require(QCoro::waitFor(database.loadCacheEntryAsync(QStringLiteral("test"), QStringLiteral("fresh"))).isEmpty(),
        "namespace invalidation should remove entries");

    database.saveCacheEntry(QStringLiteral("test"), QStringLiteral("expired"), QByteArrayLiteral("value"), 1);
    QThread::msleep(5);
    require(QCoro::waitFor(database.loadCacheEntryAsync(QStringLiteral("test"), QStringLiteral("expired"))).isEmpty(),
        "expired cache entry should not load");

    database.saveCacheEntry(QStringLiteral("test"), QStringLiteral("old"), QByteArrayLiteral("old"));
    QThread::msleep(2);
    database.saveCacheEntry(QStringLiteral("test"), QStringLiteral("new"), QByteArrayLiteral("new"));
    database.evictCacheEntries(1);
    require(QCoro::waitFor(database.loadCacheEntryAsync(QStringLiteral("test"), QStringLiteral("old"))).isEmpty(),
        "least recently used entry should be evicted");
    require(QCoro::waitFor(database.loadCacheEntryAsync(QStringLiteral("test"), QStringLiteral("new")))
            == QByteArrayLiteral("new"),
        "newest cache entry should remain");
    database.shutdown();

    QFile corruptCache(databasePath);
    require(corruptCache.open(QIODevice::WriteOnly | QIODevice::Truncate), "cache should be writable for corruption");
    corruptCache.write("not a sqlite database");
    corruptCache.close();

    DatabaseManager recovered;
    require(recovered.initialize(databasePath), "corrupt disposable cache should not block startup");
    require(QCoro::waitFor(recovered.schemaVersionAsync()) == 1, "corrupt cache should be rebuilt");
    require(QCoro::waitFor(recovered.loadSettingAsync(QStringLiteral("batch/first"))) == QStringLiteral("one"),
        "cache recovery must preserve durable settings");
    requireLegacyAccount(recovered, "cache recovery must preserve legacy sign-ins");
    recovered.shutdown();

    for (int attempt = 0; attempt < 4; ++attempt) {
        QThread::msleep(2);
        QFile repeatedCorruption(databasePath);
        require(repeatedCorruption.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "cache should remain writable for repeated corruption");
        repeatedCorruption.write("broken");
        repeatedCorruption.close();
        DatabaseManager retry;
        require(retry.initialize(databasePath), "repeated cache corruption should remain recoverable");
        require(QCoro::waitFor(retry.schemaVersionAsync()) == 1, "repeated recovery should recreate cache schema");
        retry.shutdown();
    }
    require(QDir(directory.path()).entryList({ QStringLiteral("cache.sqlite.corrupt-*") }, QDir::Files).size() == 3,
        "cache diagnostic backups should be capped");

    {
        QSqlDatabase futureState = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("future-state"));
        futureState.setDatabaseName(statePath);
        require(futureState.open(), "durable state should open for future-version seed");
        QSqlQuery query(futureState);
        require(query.exec(QStringLiteral("PRAGMA user_version = 999")), "future durable schema should be seeded");
        futureState.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("future-state"));

    DatabaseManager resetState;
    require(resetState.initialize(databasePath), "future durable state should not block startup");
    require(QCoro::waitFor(resetState.loadLegacyAccountsAsync()).isEmpty()
            && QCoro::waitFor(resetState.loadSettingAsync(QStringLiteral("batch/first"))).isEmpty(),
        "unreadable durable state should restart empty");
    resetState.shutdown();
    require(QDir(directory.path()).entryList({ QStringLiteral("state.sqlite.corrupt-*") }, QDir::Files).size() == 1,
        "future durable state should be preserved as a diagnostic backup");

    QFile::setPermissions(databasePath, QFileDevice::ReadOwner);
    QFile::setPermissions(statePath, QFileDevice::ReadOwner);
    DatabaseManager readOnly;
    require(readOnly.initialize(databasePath), "read-only storage should fall back without blocking startup");
    require(QCoro::waitFor(readOnly.schemaVersionAsync()) == 1, "read-only fallback should provide a live cache");
    readOnly.shutdown();
    QFile::setPermissions(databasePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile::setPermissions(statePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    return 0;
}
