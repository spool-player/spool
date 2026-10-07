#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "platform/CredentialStore.h"
#include "provider/Provider.h"
#include "provider/ProviderRegistry.h"
#include "provider/ProviderUiContext.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
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
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
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
QVariantMap row(const ProviderRegistry& registry, const QString& id)
{
    for (const auto& value : registry.accounts()) {
        if (value.toMap().value("id") == id)
            return value.toMap();
    }
    return {};
}
ProviderAccount saved(const ProviderRegistry& registry, const QString& id)
{
    for (const auto& entry : registry.accountList()) {
        if (entry.id == id)
            return entry;
    }
    return {};
}
ProviderPackageContents package(
    const QString& id = QStringLiteral("fixture.activation"), const QString& version = QStringLiteral("1.0.0"))
{
    auto result = ProviderFixture::package(id, version);
    auto manifest = QJsonDocument::fromJson(result.files.value("manifest.json")).object();
    auto ui = manifest.value("ui").toObject();
    ui.insert("settings", "ui/Selection.qml");
    manifest.insert("ui", ui);
    manifest.insert("capabilities", QJsonArray { "accountActivation", "suggestions", "settingsStorage" });
    result.files["manifest.json"] = QJsonDocument(manifest).toJson();
    result.manifest = *ProviderManifest::parse(result.files.value("manifest.json"));
    result.files["logic/provider.mjs"] = R"JS(
export function createSource(config, host) {
    let active = false, reason = '', lastUsed = false, hadGrant = false;
    let denied = false, socket = 0;
    let background = Promise.resolve();
    if (config.origin) {
        background = host.http(config.origin + '/must-not-start', {}).then(function() {}, function(e) {
            denied = String(e).indexOf('account_locked') >= 0;
        });
        try { host.socket(config.origin.replace('http:', 'ws:') + '/must-not-open', {}); socket = 1; }
        catch (error) { socket = String(error).indexOf('socket_denied') >= 0 ? 0 : -1; }
    }
    function check() { if (!active) throw new Error('account_locked'); }
    return {
        describe: function() {
            return {activation: {familyId: config.family || 'home', identityId: active && config.changeIdentity ? 'changed' : config.identity || 'draft'},
                capabilities: {accountActivation:true, suggestions:true, settingsStorage:true},
                artwork: active ? 'https://art.invalid/' + config.identity + '/{itemId}' : ''};
        },
        activate: function(args, operation) {
            reason = args.reason; lastUsed = args.lastUsed; hadGrant = !!args.grant;
            host.emit('configuration', {token:'candidate-' + config.identity});
            host.emit('changed', {itemId:'private-change'});
            if (config.activationFailure) throw new Error(config.activationFailure);
            if (args.reason !== 'linked' && args.reason !== 'family' && !(args.reason === 'startup' && args.lastUsed && config.automaticSignIn)) {
                if (!args.answers) return {pick:{kind:'activationPin'}};
                if (args.answers.pin !== '1234') throw new Error('invalid_pin');
            }
            if (args.reason === 'family' && (!args.grant || args.grant.identity !== config.identity))
                throw new Error('invalid_grant');
            active = true;
            return {grant: config.largeGrant ? new Array(16386).join('x') : {identity:config.identity, proof:'memory-only-grant-proof'}};
        },
        state: function() { check(); return background.then(function() { return {reason:reason,lastUsed:lastUsed,hadGrant:hadGrant,backgroundDenied:denied,socket:socket}; }); },
        suggestions: function() { check(); return {items:[],cursor:null,exhausted:true}; },
        dataInfo: function() { check(); return {maxBytes:65536,conditionalWrites:false}; },
        familyOption: function(args) { check(); host.emit('activationConfiguration', {configuration:{automaticSignIn:args.enabled !== false}}); return {}; },
        hang: function() { check(); while (true) {} },
        signOut: function() { return {}; }
    };
}
)JS";
    return result;
}
QString link(ProviderRegistry& registry, const QString& identity, const QString& server, const QString& origin = {},
    QVariantMap extra = {})
{
    auto *context = qobject_cast<ProviderUiContext *>(registry.beginSetup("fixture.activation"));
    require(context, "login context exists");
    if (!origin.isEmpty())
        QCoro::waitFor(registry.allowSetupOrigin(context->sourceId(), QUrl(origin)));
    extra.insert("identity", identity);
    extra.insert("origin", origin);
    extra.insert("token", "saved-" + identity);
    QString id;
    const auto connection
        = QObject::connect(&registry, &ProviderRegistry::accountAdded, [&id](const QString& value) { id = value; });
    context->complete({ { "account", identity + '@' + server }, { "group", server }, { "label", identity },
        { "configuration", extra } });
    waitUntil([&] { return !id.isEmpty() && registry.sourceRunning(id); },
        "linked authentication commits without another PIN");
    QObject::disconnect(connection);
    return id;
}
}

SPOOL_TEST_MAIN("provider-activation")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath("cache.sqlite")), "database opens");
    QCoro::waitFor(database.schemaVersionAsync());
    const auto installs = directory.filePath("providers");
    require(ProviderPackage::install(package(), installs).has_value(), "activation fixture installs");
    require(ProviderPackage::install(package("fixture.isolated"), installs).has_value(),
        "isolated activation module installs");
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "loopback listens");
    const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    int backgroundConnections = 0;
    QObject::connect(&server, &QTcpServer::newConnection, [&] { ++backgroundConnections; });
    QString a, a2, b;
    {
        ProviderRegistry registry(&database);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        SourceHub hub(&registry);
        QCoro::waitFor(registry.restore());
        QPointer<ProviderUiContext> picker;
        int prompts = 0;
        int failures = 0;
        QList<QPair<QString, bool>> selections;
        QObject::connect(&registry, &ProviderRegistry::accountSelectionFinished,
            [&](const QString& id, bool selected) { selections.append({ id, selected }); });
        QObject::connect(&registry, &ProviderRegistry::problem, [&](const QString&) { ++failures; });
        QStringList revoked;
        QObject::connect(&registry, &ProviderRegistry::componentRequested, [&](QObject *context) {
            picker = qobject_cast<ProviderUiContext *>(context);
            ++prompts;
        });
        QObject::connect(&registry, &ProviderRegistry::accountIdentityRevoked, [&](const QString& id) {
            require(hub.source(id), "revocation precedes source detachment");
            revoked.append(id);
        });
        a = link(registry, "a", "one", origin);
        const auto initial = QCoro::waitFor(registry.callSource(a, "state"));
        require(initial.value("reason") == "linked" && initial.value("backgroundDenied").toBool()
                && initial.value("socket").toInt() == 0 && backgroundConnections == 0 && prompts == 0,
            "fresh authentication is private and denies background HTTP/sockets before publication");
        require(failure(registry.callSource(a, "activate", { { "reason", "linked" } })) == "action_unavailable",
            "public calls cannot claim the fresh-link bypass");
        a2 = registry.finishSetup({},
            { { "module", "fixture.activation" }, { "account", "a@two" }, { "group", "two" }, { "label", "a" },
                { "configuration", QVariantMap { { "identity", "a" } } } });
        waitUntil([&] { return registry.sourceRunning(a2); }, "same identity activates another server");
        require(registry.sourceRunning(a)
                && QCoro::waitFor(registry.callSource(a2, "state")).value("reason") == "family" && prompts == 0,
            "family proof preserves same-identity servers without a second prompt");
        const QString isolated = registry.finishSetup({},
            { { "module", "fixture.isolated" }, { "account", "a@one" }, { "group", "one" }, { "label", "a" },
                { "configuration", QVariantMap { { "identity", "a" } } } });
        waitUntil(
            [&] { return picker && !picker->closed(); }, "another module cannot reuse an equal family/identity proof");
        picker->close();
        waitUntil([&] { return row(registry, isolated).value("connectionState") == "locked"; },
            "isolated module stays locked");
        require(registry.sourceRunning(a) && registry.sourceRunning(a2),
            "module-local identity boundaries preserve unrelated sources");
        picker = nullptr;
        b = link(registry, "b", "one");
        require(
            !registry.sourceRunning(a) && !registry.sourceRunning(a2) && revoked.contains(a) && revoked.contains(a2),
            "successful identity switch revokes all old identity servers");
        const auto previous = saved(registry, b);
        const auto credential = saved(registry, a).configuration;
        const int revocations = revoked.size();
        require(!registry.sourceRunning(a) && !registry.sourceRunning(a2)
                && failure(registry.callSource(a, "state")) == "source_unavailable"
                && failure(registry.callSource(a, "dataInfo")) == "unsupported_capability",
            "search and sync cannot implicitly activate another Home identity");
        selections.clear();
        registry.useAccount(a);
        waitUntil([&] { return picker && !picker->closed(); }, "switch asks for a PIN");
        const QString privateId = picker->sourceId();
        require(!registry.sourceRunning(privateId) && !registry.openPicker(a, {}) && !registry.openSettings(a),
            "private source and locked identity expose no regular UI or account operations");
        require(failure(registry.callSource(privateId, "state")) == "source_unavailable"
                && failure(registry.callSource(privateId, "activate")) == "action_unavailable",
            "even a private picker source ID cannot bypass the activation transaction");
        require(row(registry, a).value("pendingEnabled").toBool()
                && std::none_of(selections.begin(), selections.end(),
                    [&](const auto& result) { return result.first == a && result.second; }),
            "pending PIN never reports a completed successful selection");
        picker->close();
        waitUntil([&] { return row(registry, a).value("connectionState") == "locked"; }, "cancel settles locked");
        require(!selections.isEmpty() && selections.back() == qMakePair(a, false),
            "cancelled switch reports failure instead of navigating home");
        require(registry.sourceRunning(b) && saved(registry, b).lastUsed == previous.lastUsed
                && saved(registry, a).configuration == credential && revoked.size() == revocations,
            "cancellation preserves previous active source, credentials and last-used selection");
        picker = nullptr;
        registry.setAccountEnabled(a, true);
        waitUntil([&] { return picker && !picker->closed(); }, "enable also requires activation");
        picker->complete({ { "pin", "bad" }, { "reason", "linked" }, { "lastUsed", true }, { "grant", "spoof" } });
        waitUntil([&] { return row(registry, a).value("connectionState") == "locked"; }, "bad PIN fails closed");
        require(registry.sourceRunning(b) && saved(registry, a).configuration == credential
                && !saved(registry, a).enabled && revoked.size() == revocations,
            "nested picker answers cannot overwrite reason/proof or mutate candidate credentials");
        picker = nullptr;
        registry.useAccount(a);
        waitUntil([&] { return picker && !picker->closed(); }, "retry prompts afresh");
        picker->complete({ { "pin", "1234" } });
        waitUntil([&] { return registry.sourceRunning(a); }, "valid PIN commits");
        require(selections.back() == qMakePair(a, true), "selection succeeds only after authorized publication");
        require(!registry.sourceRunning(b) && !registry.sourceRunning(a2),
            "only authorized selected identity becomes active");
        registry.useAccount(a2);
        waitUntil([&] { return registry.sourceRunning(a2); }, "grant is reusable across saved servers");
        require(QCoro::waitFor(registry.callSource(a2, "state")).value("reason") == "family",
            "cached proof is identity scoped");
        const auto stable = saved(registry, a2).configuration;
        picker = nullptr;
        const int beforeGrantFailure = failures;
        registry.restartAccount(a2, { { "largeGrant", true } });
        waitUntil([&] { return picker && !picker->closed(); }, "restart clears prior generation proof");
        picker->complete({ { "pin", "1234" } });
        waitUntil([&] { return failures == beforeGrantFailure + 1; }, "oversized scalar proof is rejected");
        require(saved(registry, a2).configuration == stable && registry.sourceRunning(a2),
            "a scalar grant over 16 KiB cannot replace the current generation");
        picker = nullptr;
        const int beforeIdentityFailure = failures;
        registry.restartAccount(a2, { { "changeIdentity", true } });
        waitUntil([&] { return picker && !picker->closed(); }, "identity-changing candidate asks for PIN");
        picker->complete({ { "pin", "1234" } });
        waitUntil(
            [&] { return failures == beforeIdentityFailure + 1; }, "changed identity is rejected before publication");
        require(saved(registry, a2).configuration == stable && registry.sourceRunning(a2),
            "a saved account cannot become a different identity after activation");
        const int beforeStaleFailure = failures;
        registry.restartAccount(a2, { { "activationFailure", "http_401" } });
        waitUntil([&] { return failures == beforeStaleFailure + 1; }, "stale credentials reject replacement");
        require(row(registry, a2).value("needsSignIn").toBool()
                && !row(registry, a2).value("errorText").toString().isEmpty()
                && saved(registry, a2).configuration == stable && registry.sourceRunning(a2),
            "failed authentication offers reconnect without committing candidate credentials or viewer");
        int optionNotifications = 0;
        auto *optionContext = qobject_cast<ProviderUiContext *>(registry.openSettings(a2));
        require(optionContext, "active sibling can open its provider screen");
        QObject::connect(
            optionContext, &ProviderUiContext::activationConfigurationChanged, [&] { ++optionNotifications; });
        QCoro::waitFor(registry.callSource(a, "familyOption"));
        waitUntil([&] { return saved(registry, a2).configuration.value("automaticSignIn").toBool(); },
            "authenticated family option reaches same-family server accounts locally");
        require(optionNotifications > 0 && optionContext->activationConfiguration().value("automaticSignIn").toBool(),
            "open sibling screens observe current family options without restarting their provider");
        optionContext->close();
        delete optionContext;
    }
    require(!CredentialStore::load(a).contains("memory-only-grant-proof")
            && !CredentialStore::load(a2).contains("memory-only-grant-proof"),
        "activation grants never persist");
    // Old metadata must not make an unprotected or protected Home sibling searchable.
    auto metadata
        = QJsonDocument::fromJson(QCoro::waitFor(database.loadSettingAsync("providers/accounts/2")).toUtf8()).object();
    auto accounts = metadata.value("accounts").toArray();
    for (qsizetype i = 0; i < accounts.size(); ++i) {
        auto entry = accounts.at(i).toObject();
        if (entry.value("id").toString() == b) {
            entry.remove("activationFamily");
            entry.remove("activationIdentity");
            accounts[i] = entry;
        }
    }
    metadata.insert("accounts", accounts);
    database.saveSetting(
        "providers/accounts/2", QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
    QCoro::waitFor(database.loadSettingAsync("providers/accounts/2"));
    {
        ProviderRegistry registry(&database);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        int prompts = 0;
        QObject::connect(&registry, &ProviderRegistry::componentRequested, [&](QObject *) { ++prompts; });
        QCoro::waitFor(registry.restore());
        waitUntil([&] { return registry.sourceRunning(a2) && registry.sourceRunning(a); },
            "automatic last-used startup authorizes same-identity siblings");
        require(QCoro::waitFor(registry.callSource(a2, "state")).value("reason") == "startup"
                && QCoro::waitFor(registry.callSource(a, "state")).value("reason") == "family" && prompts == 0
                && !registry.sourceRunning(b),
            "startup option is last-used-only; previous process proof is absent");
        QPointer<ProviderUiContext> picker;
        QObject::connect(&registry, &ProviderRegistry::componentRequested,
            [&](QObject *value) { picker = qobject_cast<ProviderUiContext *>(value); });
        require(!registry.sourceRunning(b) && prompts == 0,
            "alternate Home profiles stay stopped and cannot expand search permissions");
        registry.useAccount(b);
        waitUntil(
            [&] { return picker && !picker->closed(); }, "automatic sign-in never bypasses explicit identity switch");
        registry.removeAccount(b);
        waitUntil([&] { return !picker || picker->closed(); }, "removing a prepared source closes its pending picker");
        require(registry.sourceRunning(a) && registry.sourceRunning(a2),
            "cancelled removal cannot revoke current identity");
        QCoro::waitFor(registry.callSource(a, "familyOption", { { "enabled", false } }));
        waitUntil([&] { return !saved(registry, a2).configuration.value("automaticSignIn").toBool(); },
            "automatic option can be disabled locally");
    }
    {
        ProviderRegistry registry(&database);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        QPointer<ProviderUiContext> picker;
        int prompts = 0;
        QObject::connect(&registry, &ProviderRegistry::componentRequested, [&](QObject *context) {
            picker = qobject_cast<ProviderUiContext *>(context);
            ++prompts;
        });
        QCoro::waitFor(registry.restore());
        waitUntil(
            [&] {
                return row(registry, a).value("connectionState") == "locked"
                    && row(registry, a2).value("connectionState") == "locked";
            },
            "protected startup quietly settles locked");
        require(!registry.sourceRunning(a) && !registry.sourceRunning(a2) && prompts == 0,
            "startup neither publishes protected media nor opens a profile/PIN chooser");
        registry.useAccount(a2);
        waitUntil([&] { return picker && !picker->closed(); }, "explicit profile selection requests the PIN");
        picker->close();
        waitUntil([&] { return row(registry, a2).value("connectionState") == "locked"; },
            "explicit cancellation keeps protected media locked");
    }
    {
        std::atomic_int socketAttempts { 0 };
        ProviderRegistry registry(&database);
        ScriptRuntime::NetworkHooks hooks;
        hooks.socket = [&](QWebSocket *, QUrl) { ++socketAttempts; };
        registry.setRuntimeEnvironment({}, hooks);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        SourceHub hub(&registry);
        QPointer<ProviderUiContext> picker;
        QStringList revoked;
        int publications = 0;
        QObject::connect(&registry, &ProviderRegistry::sourceStarted, [&](Provider *) { ++publications; });
        QObject::connect(&registry, &ProviderRegistry::componentRequested,
            [&](QObject *context) { picker = qobject_cast<ProviderUiContext *>(context); });
        QObject::connect(&registry, &ProviderRegistry::accountIdentityRevoked, [&](const QString& id) {
            require(hub.source(id), "destructive revocation is delivered before old identity detaches");
            revoked.append(id);
        });
        QCoro::waitFor(registry.restore());
        const auto authorizeSaved = [&] {
            waitUntil(
                [&] {
                    return row(registry, a).value("connectionState") == "locked"
                        && row(registry, a2).value("connectionState") == "locked";
                },
                "background restore leaves protected viewers locked");
            picker = nullptr;
            registry.useAccount(a);
            waitUntil([&] { return picker && !picker->closed(); }, "explicit viewer selection requests authorization");
            picker->complete({ { "pin", "1234" } });
            waitUntil([&] { return registry.sourceRunning(a); }, "selected server activates");
            registry.useAccount(a2);
            waitUntil([&] { return registry.sourceRunning(a2); }, "authorized same-viewer sibling activates");
        };
        authorizeSaved();
        const QPointer<Provider> previousSource = hub.source(a2);
        picker = nullptr;
        registry.restartAccount(a2);
        waitUntil(
            [&] { return picker && !picker->closed(); }, "private same-identity replacement obtains authorization");
        require(hub.source(a2) == previousSource && registry.sourceRunning(a) && revoked.isEmpty(),
            "private replacement keeps existing identity sessions available while awaiting a PIN");
        picker->complete({ { "pin", "1234" } });
        waitUntil([&] { return hub.source(a2) && hub.source(a2) != previousSource.data(); },
            "private candidate replaces only its source");
        require(revoked.isEmpty() && registry.sourceRunning(a),
            "successful same-identity replacement does not revoke local playback or sibling accounts");
        picker = nullptr;
        QCoro::waitFor(registry.install(package("fixture.activation", "1.1.0")));
        require(revoked.count(a) == 1 && revoked.count(a2) == 1 && !registry.sourceRunning(a)
                && !registry.sourceRunning(a2),
            "module replacement revokes every published identity server before leaving them locked");
        authorizeSaved();
        require(!failure(registry.callSource(a2, "hang")).isEmpty(), "watchdog interrupts the stalled operation");
        waitUntil([&] { return revoked.count(a) == 2 && revoked.count(a2) == 2; },
            "watchdog revokes all published identity sources before stopping the module");
        require(!registry.sourceRunning(a) && !registry.sourceRunning(a2)
                && failure(registry.callSource(a, "state")) == "source_unavailable",
            "interrupted identity sources cannot supply old authenticated media");
        picker = nullptr;
        QCoro::waitFor(registry.install(package("fixture.activation", "1.2.0")));
        authorizeSaved();
        const int authorizedPublications = publications;
        for (const bool missingDescription : { false, true }) {
            auto downgraded = package("fixture.activation", missingDescription ? "1.4.0" : "1.3.0");
            auto manifest = QJsonDocument::fromJson(downgraded.files.value("manifest.json")).object();
            auto declarations = manifest.value("capabilities").toArray();
            for (qsizetype index = 0; index < declarations.size(); ++index) {
                if (declarations.at(index).toString() == QStringLiteral("accountActivation")) {
                    declarations.removeAt(index);
                    break;
                }
            }
            manifest.insert("capabilities", declarations);
            downgraded.files["manifest.json"] = QJsonDocument(manifest).toJson();
            downgraded.manifest = *ProviderManifest::parse(downgraded.files.value("manifest.json"));
            if (missingDescription) {
                downgraded.files["logic/provider.mjs"] = R"JS(
export function createSource(config, host) {
    if (config.origin) {
        host.http(config.origin + '/legacy-startup', {}).then(function() {}, function() {});
        try { host.socket(config.origin.replace('http:', 'ws:') + '/legacy-socket', {}); } catch (error) {}
    }
    return {describe:function() { return {}; }};
}
)JS";
            }
            QCoro::waitFor(registry.install(std::move(downgraded)));
            waitUntil(
                [&] {
                    return row(registry, a).value("connectionState") == "locked"
                        && row(registry, a2).value("connectionState") == "locked";
                },
                "saved protected identities reject activation capability removal");
            require(publications == authorizedPublications && socketAttempts.load() == 0 && backgroundConnections == 0
                    && !registry.sourceRunning(a) && !registry.sourceRunning(a2),
                "removing activation declaration or metadata cannot start authenticated background I/O or publish "
                "saved protected accounts");
        }
        picker = nullptr;
        QCoro::waitFor(registry.install(package("fixture.activation", "1.5.0")));
        authorizeSaved();
        QCoro::waitFor(registry.uninstall("fixture.activation"));
        require(revoked.count(a) == 4 && revoked.count(a2) == 4 && !hub.source(a) && !hub.source(a2),
            "uninstall revokes published identities before removing their source and account metadata");
    }
    return 0;
}
