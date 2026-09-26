#pragma once

#include "ProviderMediaPage.h"

#include <QCoroTask>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QWebSocket;

namespace Spool {

// One provider module on its own worker thread and QJSEngine (API 0.2). JS
// values never leave the worker; results are converted to native values
// there. The host picks source IDs and the origins each source may reach.
class ScriptRuntime final : public QObject {
    Q_OBJECT
public:
    // Runs on the worker thread for its network objects, so TLS trust
    // decisions the viewer made apply to provider traffic too.
    struct NetworkHooks {
        std::function<void(QNetworkAccessManager *)> network;
        std::function<void(QWebSocket *, QUrl)> socket;
    };

    ScriptRuntime(QString entryPoint, QVariantMap device, NetworkHooks hooks = {}, QObject *parent = nullptr);
    ~ScriptRuntime() override;

    // An origin of "*" lets the source reach any HTTP(S) origin.
    QCoro::Task<QVariantMap> addSource(QString sourceId, QVariantMap configuration, QList<QUrl> origins);
    QCoro::Task<QVariantMap> call(QString sourceId, QString method, QVariantMap arguments = {}, QString scope = {});
    // Listing path: decoded on the worker straight into native items.
    QCoro::Task<ProviderMediaPage> callMediaPage(
        QString sourceId, QString method, QVariantMap arguments = {}, QString scope = {}, int maximumItems = 100);
    // Details path: `{item: {...}}` decoded on the worker.
    QCoro::Task<MovieItem> callItem(QString sourceId, QString method, QVariantMap arguments = {});
    void cancelScope(const QString& sourceId, const QString& scope);
    void removeSource(const QString& sourceId);

signals:
    // host.emit(type, payload) from a source, delivered on the owner's thread.
    void event(const QString& sourceId, const QString& type, const QVariantMap& payload);
    // The module overran its execution budget and every source is gone.
    void interrupted();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace Spool
