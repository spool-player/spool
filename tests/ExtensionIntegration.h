#pragma once

#include "app/LocalizationManager.h"
#include "app/RemoteTargetsController.h"
#include "app/SettingsController.h"
#include "app/SettingsSyncController.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderCapabilities.h"
#include "provider/ProviderRegistry.h"
#include "provider/ProviderUiContext.h"
#include "provider/SourceHub.h"
#include "providers/ProviderFixture.h"

#include <QJsonDocument>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

// Instantiated only by the dedicated integration test. No user accounts,
// settings, external network, playback devices or desktop windows are used.
class ExtensionIntegration final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString first MEMBER first CONSTANT)
    Q_PROPERTY(QString second MEMBER second CONSTANT)
    Q_PROPERTY(QString protectedAccount MEMBER protectedAccount NOTIFY changed)
    Q_PROPERTY(QObject *picker READ picker NOTIFY changed)
    Q_PROPERTY(int cycles MEMBER cycles NOTIFY changed)
public:
    explicit ExtensionIntegration(QObject *parent = nullptr)
        : QObject(parent)
    {
        if (!directory.isValid() || !server.listen(QHostAddress::LocalHost))
            qFatal("integration fixture could not initialize");
        qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
        if (!database.initialize(directory.filePath("cache.sqlite")))
            qFatal("integration database could not initialize");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const auto split = buffer->indexOf("\r\n\r\n");
                    if (split < 0)
                        return;
                    int length = 0;
                    for (const auto& header : buffer->left(split).split('\n'))
                        if (header.toLower().startsWith("content-length:"))
                            length = header.mid(15).trimmed().toInt();
                    if (buffer->size() < split + 4 + length)
                        return;
                    const auto path = QString::fromUtf8(buffer->split(' ').value(1)).split('/');
                    const auto args = QJsonDocument::fromJson(buffer->mid(split + 4, length)).object().toVariantMap();
                    const auto operation = path.value(2);
                    const auto response = dispatch(path.value(1), operation, args);
                    const QByteArray body = QJsonDocument::fromVariant(response).toJson(QJsonDocument::Compact);
                    const bool fail = failStorage && operation == "dataRead";
                    const auto send = [guard = QPointer<QTcpSocket>(socket), body, fail] {
                        if (!guard)
                            return;
                        guard->write(QByteArray(fail ? "HTTP/1.1 503 Unavailable\r\n" : "HTTP/1.1 200 OK\r\n")
                            + "Content-Type: application/json\r\nConnection: close\r\nContent-Length: "
                            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        guard->disconnectFromHost();
                    };
                    socket->disconnect(this);
                    if (operation == "dataRead" && delayRead > 0)
                        QTimer::singleShot(delayRead, this, send);
                    else
                        send();
                });
            }
        });
        auto package = ProviderFixture::package("fixture.integration");
        auto manifest = QJsonDocument::fromJson(package.files["manifest.json"]).object();
        manifest["origins"] = QJsonArray { QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()) };
        manifest["extensions"] = QJsonObject { { "spool.settings-storage", 1 }, { "spool.playback-preferences", 1 },
            { "spool.remote-targets", 1 }, { "spool.account-activation", 1 } };
        package.files["manifest.json"] = QJsonDocument(manifest).toJson();
        package.manifest = *Spool::ProviderManifest::parse(package.files["manifest.json"]);
        for (const auto& file : { QString("extension-integration.mjs"), QString("IntegrationPin.qml") }) {
            QFile input(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/") + file);
            if (!input.open(QIODevice::ReadOnly))
                qFatal("integration fixture missing");
            package.files[file.endsWith("mjs") ? "logic/provider.mjs" : "ui/Selection.qml"] = input.readAll();
        }
        const auto installs = directory.filePath("providers");
        if (!Spool::ProviderPackage::install(package, installs))
            qFatal("integration fixture installation failed");
        registry = std::make_unique<Spool::ProviderRegistry>(&database);
        registry->setInstallDirectory(installs);
        registry->loadModules();
        hub = std::make_unique<Spool::SourceHub>(registry.get());
        QCoro::waitFor(registry->restore());
        connect(registry.get(), &Spool::ProviderRegistry::componentRequested, this, [this](QObject *value) {
            context = value;
            emit changed();
        });
        first = add("first");
        second = add("second");
        settings = std::make_unique<Spool::SettingsController>(&database, nullptr, nullptr);
        settings->attachLocalization(&localization);
        sync = std::make_unique<Spool::SettingsSyncController>(settings.get(), &database, registry.get());
        settings->attachSync(sync.get());
        connect(sync.get(), &Spool::SettingsSyncController::cycleFinished, this, [this] {
            ++cycles;
            emit changed();
        });
        remote = std::make_unique<Spool::RemoteTargetsController>(hub.get(), registry.get(), nullptr);
        QCoro::waitFor(settings->loadLocalAsync());
        QCoro::waitFor(sync->loadLocalAsync());
    }
    void expose(QQmlEngine *engine)
    {
        auto *context = engine->rootContext();
        context->setContextProperty("Integration", this);
        context->setContextProperty("Settings", settings.get());
        context->setContextProperty("SettingsSync", sync.get());
        context->setContextProperty("Providers", registry.get());
        context->setContextProperty("RemoteTargets", remote.get());
        context->setContextProperty("I18n", &localization);
        auto *placeholders = QQmlPropertyMap::create(this);
        placeholders->insert("hdrPlayback", false);
        placeholders->insert("sessionActive", false);
        placeholders->insert("speedTest", false);
        placeholders->insert("updates", QVariantList {});
        placeholders->insert("connectionSpeedDescription", QString());
        context->setContextProperty("ProviderCapabilities", new Spool::ProviderCapabilities(this));
        for (const auto *name : { "Player", "Store", "App" })
            context->setContextProperty(name, placeholders);
    }
    Q_INVOKABLE QVariantMap stats(QString identity) const
    {
        return states.value(identity);
    }
    Q_INVOKABLE void setReadDelay(int delay)
    {
        delayRead = delay;
    }
    Q_INVOKABLE void setStorageFailure(bool fail)
    {
        failStorage = fail;
    }
    Q_INVOKABLE void startProtected()
    {
        protectedAccount = add("protected", true);
        emit changed();
    }
    Q_INVOKABLE bool running(QString id) const
    {
        return registry->sourceRunning(id);
    }
    Q_INVOKABLE QString capturePath(QString name) const
    {
        const auto path = qEnvironmentVariable("SPOOL_INTEGRATION_CAPTURES");
        if (path.isEmpty())
            return {};
        QDir().mkpath(path);
        return QDir(path).filePath(name + ".png");
    }
    QObject *picker() const
    {
        return context;
    }
signals:
    void changed();

private:
    QString add(const QString& identity, bool protectedSource = false)
    {
        const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        const auto id = registry->finishSetup({},
            { { "module", "fixture.integration" }, { "account", identity }, { "label", identity },
                { "origins", QVariantList { origin } },
                { "configuration",
                    QVariantMap {
                        { "identity", identity }, { "server", origin }, { "protected", protectedSource } } } });
        registry->useAccount(id);
        return id;
    }
    QVariantMap dispatch(const QString& identity, const QString& operation, const QVariantMap& args)
    {
        auto& state = states[identity];
        if (state.isEmpty())
            state = { { "native",
                          QVariantMap { { "audioLanguage", "eng" }, { "audioMode", "Default" },
                              { "subtitleLanguage", "eng" }, { "subtitleMode", "Default" } } },
                { "writes", 0 },
                { "queue",
                    QVariantList { QVariantMap { { "id", "movie" }, { "title", "Film" }, { "type", "Movie" },
                                       { "entryId", "one" } },
                        QVariantMap {
                            { "id", "movie" }, { "title", "Film" }, { "type", "Movie" }, { "entryId", "two" } } } } };
        if (operation == "preferencesRead")
            return { { "values", state["native"] },
                { "writable", QVariantList { "audioLanguage", "audioMode", "subtitleLanguage", "subtitleMode" } } };
        if (operation == "preferencesWrite") {
            auto values = state["native"].toMap();
            values.insert(args.value("values").toMap());
            state["native"] = values;
        } else if (operation == "dataInfo")
            return { { "maxBytes", 65536 }, { "conditionalWrites", false } };
        else if (operation == "dataRead")
            return state.contains("document") ? QVariantMap { { "found", true }, { "value", state.value("document") } }
                                              : QVariantMap { { "found", false } };
        else if (operation == "dataWrite") {
            state["document"] = args.value("value");
            state["writes"] = state["writes"].toInt() + 1;
        } else if (operation == "remoteTargets")
            return { { "targets",
                QVariantList { QVariantMap { { "id", "tv" }, { "name", "Loopback TV" },
                    { "commands", QVariantList { "pause", "queueRemove" } }, { "queueEditing", "in-place" } } } } };
        else if (operation == "remoteState")
            return { { "state", "playing" }, { "commands", QVariantList { "pause", "queueRemove" } },
                { "positionTicks", "100000000" }, { "runtimeTicks", "1000000000" },
                { "queueRevision", QString::number(commands) } };
        else if (operation == "remoteQueue")
            return { { "items", state["queue"] }, { "cursor", QVariant::fromValue(nullptr) }, { "exhausted", true } };
        else if (operation == "remoteCommand") {
            ++commands;
            const auto command = args.value("command").toMap();
            if (command.value("action") == "queueRemove") {
                auto queue = state["queue"].toList();
                for (qsizetype i = queue.size(); i-- > 0;)
                    if (queue[i].toMap().value("entryId") == command.value("entryId"))
                        queue.removeAt(i);
                state["queue"] = queue;
            }
        }
        return {};
    }
    QTemporaryDir directory;
    QTcpServer server;
    Spool::DatabaseManager database;
    Spool::LocalizationManager localization;
    QHash<QString, QVariantMap> states;
    std::unique_ptr<Spool::ProviderRegistry> registry;
    std::unique_ptr<Spool::SourceHub> hub;
    std::unique_ptr<Spool::SettingsController> settings;
    std::unique_ptr<Spool::SettingsSyncController> sync;
    std::unique_ptr<Spool::RemoteTargetsController> remote;
    QPointer<QObject> context;
    QString first, second, protectedAccount;
    int cycles = 0, delayRead = 0, commands = 0;
    bool failStorage = false;
};
