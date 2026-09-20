#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
template <typename T> void rejects(QCoro::Task<T> task, const char *message)
{
    bool failed = false;
    try {
        QCoro::waitFor(std::move(task));
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, message);
}
}

JELLYFIN_TEST_MAIN("source-registry")
{
    QCoreApplication app(argc, argv);
    using namespace JellyfinNative;
    QTemporaryDir directory;
    require(directory.isValid(), "isolated persistent state directory created");
    qputenv("JELLYFIN_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath("cache.sqlite")), "source metadata uses existing durable state");
    const QString entry = QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/provider.mjs");
    const QString secret = QStringLiteral("private-source-token-not-in-metadata");
    AccountProfile account;
    account.profileId = QStringLiteral("account-a");
    account.serverId = QStringLiteral("server");
    account.serverUrl = QStringLiteral("https://fixture.invalid");
    account.userId = QStringLiteral("user-a");
    account.accessToken = secret;
    database.upsertAccountProfile(account);
    const auto configure
        = [&](ProviderRegistry& registry, const QString& module, const QString& account, const QString& label) {
              return registry.configureSource(module, account, "server-key", label,
                  { { "label", label }, { "token", secret } }, { QUrl("https://fixture.invalid") });
          };
    QString a, b, c;
    {
        ProviderRegistry registry;
        registry.registerModule("fixture.one", entry);
        registry.registerModule("fixture.two", entry);
        QCoro::waitFor(registry.restoreSources(&database));
        a = QCoro::waitFor(configure(registry, "fixture.one", "account-a", "A"));
        b = QCoro::waitFor(configure(registry, "fixture.one", "account-b", "B"));
        c = QCoro::waitFor(configure(registry, "fixture.two", "account-a", "C"));
        require(a != b && a != c && b != c, "module and account scopes have distinct persistent source IDs");
        auto first = registry.callSource(a, "bump");
        auto second = registry.callSource(b, "bump");
        auto third = registry.callSource(c, "bump");
        require(QCoro::waitFor(std::move(first)).value("label") == "A", "first source has its own factory context");
        require(
            QCoro::waitFor(std::move(second)).value("label") == "B", "second account is not an active-provider switch");
        require(QCoro::waitFor(std::move(third)).value("label") == "C", "another module runs independently");
        auto pending = registry.callSource(a, "delay", { { "milliseconds", 10000 } });
        QCoro::waitFor(registry.setSourceEnabled(a, false));
        rejects(std::move(pending), "disabling a source cancels its pending operation");
        rejects(registry.callSource(a, "bump"), "disabled source cannot be used");
        require(QCoro::waitFor(registry.callSource(b, "bump")).value("calls").toInt() == 2,
            "disabling A leaves B's context and state unchanged");
        const QString renamed = QCoro::waitFor(configure(registry, "fixture.one", "account-b", "Renamed B"));
        require(renamed == b, "configuration or display-name changes do not reidentify a source");
        const auto publicData = QJsonDocument::fromVariant(registry.configuredSources()).toJson();
        const auto persisted = QCoro::waitFor(database.loadSettingAsync("portable/sources/1"));
        require(!publicData.contains(secret.toUtf8()) && !persisted.contains(secret),
            "neither UI metadata nor durable source index contains credentials");
        require(!persisted.contains("fixture.invalid"), "authorised resource addresses are not source identity");
        rejects(registry.callSource(b, "spin"), "a failing module is interrupted");
        rejects(registry.callSource(b, "bump"), "interrupted module contexts are unavailable");
        require(QCoro::waitFor(registry.callSource(c, "bump")).value("calls").toInt() == 2,
            "module failure is contained and does not reset another module");
    }
    {
        ProviderRegistry restored;
        restored.registerModule("fixture.one", entry);
        restored.registerModule("fixture.two", entry);
        QCoro::waitFor(restored.restoreSources(&database));
        require(restored.configuredSources().size() == 3,
            "all configured identities survive process-level registry recreation");
        rejects(restored.callSource(b, "bump"),
            "restoring public metadata does not invent credentials or activate a source");
        require(QCoro::waitFor(configure(restored, "fixture.one", "account-a", "A")) == a,
            "disabled source preserves its identity across restarts");
        rejects(restored.callSource(a, "bump"), "disable preference survives credential restoration");
        QCoro::waitFor(restored.setSourceEnabled(a, true));
        require(QCoro::waitFor(restored.callSource(a, "bump")).value("calls").toInt() == 1,
            "reenabling starts a clean execution context under the same persistent ID");
        require(QCoro::waitFor(configure(restored, "fixture.one", "account-b", "B")) == b,
            "second account restores the same persistent ID");
        require(QCoro::waitFor(configure(restored, "fixture.two", "account-a", "C")) == c,
            "other module preserves its separate account namespace");
        QCoro::waitFor(restored.removeSource(a));
        rejects(restored.callSource(a, "bump"), "removed source handles cannot be reused");
        require(QCoro::waitFor(restored.callSource(c, "bump")).value("label") == "C",
            "removal does not affect another module");
    }
    {
        ProviderRegistry restored;
        QCoro::waitFor(restored.restoreSources(&database));
        require(restored.configuredSources().size() == 2,
            "source deletion is durable independently of module availability");
    }
    const auto accounts = QCoro::waitFor(database.loadAccountProfilesAsync());
    require(accounts.size() == 1 && accounts.front().accessToken == secret,
        "removing a source does not remove its account or credentials");
    const QString incompatible = QStringLiteral("{\"format\":2,\"sources\":[]}");
    database.saveSetting("portable/sources/1", incompatible);
    ProviderRegistry incompatibleRegistry;
    rejects(incompatibleRegistry.restoreSources(&database), "future authoritative source state fails closed");
    require(QCoro::waitFor(database.loadSettingAsync("portable/sources/1")) == incompatible,
        "incompatible authoritative state is retained rather than wiped as a cache");
    return 0;
}
