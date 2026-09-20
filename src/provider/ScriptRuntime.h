#pragma once

#include <QCoroTask>
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <memory>

namespace JellyfinNative {

// Experimental provider API 0.1. JS values never leave the worker. The host
// chooses persistent source IDs and authorised origins, not provider code.
class ScriptRuntime final : public QObject {
    Q_OBJECT
public:
    explicit ScriptRuntime(QString entryPoint, QObject *parent = nullptr);
    ~ScriptRuntime() override;

    QCoro::Task<QVariantMap> addSource(QString sourceId, QVariantMap configuration, QList<QUrl> origins);
    QCoro::Task<QVariantMap> call(QString sourceId, QString method, QVariantMap arguments = {});
    void removeSource(const QString& sourceId);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace JellyfinNative
