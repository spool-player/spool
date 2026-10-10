#pragma once

#include "../provider/DownloadSource.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <memory>

class QNetworkAccessManager;
namespace Spool {
class SourceHub;
class AndroidDownloadStorage;
class TlsTrustController;

class DownloadManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
    Q_PROPERTY(QString destination READ destination NOTIFY changed)
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool enabled READ enabled NOTIFY enabledChanged)
    Q_PROPERTY(bool mobile READ mobile CONSTANT)
    Q_PROPERTY(bool canChooseFolder READ canChooseFolder CONSTANT)
    Q_PROPERTY(QVariantList destinationChoices READ destinationChoices CONSTANT)
    Q_PROPERTY(bool opened READ opened NOTIFY openedChanged)
    Q_PROPERTY(QString selectionItemId READ selectionItemId NOTIFY openedChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
public:
    DownloadManager(
        SourceHub *sources, QString dataRoot, TlsTrustController *trust = nullptr, QObject *parent = nullptr);
    ~DownloadManager() override;
    QVariantList jobs() const;
    QString destination() const
    {
        return m_destination;
    }
    QVariantList libraryFiles() const;
    static bool supported();
    bool enabled() const
    {
        return m_enabled;
    }
    void setEnabled(bool enabled);
    static bool mobile();
    static bool canChooseFolder();
    static QVariantList destinationChoices();
    bool opened() const
    {
        return m_opened;
    }
    QString selectionItemId() const
    {
        return m_selectionItemId;
    }
    QString problem() const
    {
        return m_problem;
    }
    Q_INVOKABLE void open(const QString& itemId = {});
    Q_INVOKABLE QVariantList folderEntries(const QString& path) const;
    Q_INVOKABLE void close();
    Q_INVOKABLE void start(const QString& itemId, const QVariantMap& option);
    Q_INVOKABLE void cancel(const QString& id);
    Q_INVOKABLE void retry(const QString& id);
    Q_INVOKABLE void remove(const QString& id);
    Q_INVOKABLE void setDestination(const QUrl& folder);
    Q_INVOKABLE void resetDestination();
    Q_INVOKABLE void chooseFolder();
    Q_INVOKABLE QString offlineItemId(const QString& id) const;
    Q_INVOKABLE QVariantMap statusFor(const QString& itemId) const;
    Q_INVOKABLE void cancelAll();
    Q_INVOKABLE void clearFinished();
signals:
    void changed();
    void enabledChanged();
    void openedChanged();
    void libraryChanged();
    void toastRequested(const QString& message);

private:
    struct Job;
    QCoro::Task<void> prepare(std::shared_ptr<Job> job);
    // Releasing an acquired plan must not depend on the manager's lifetime:
    // the provider hub owns the acquisition and its normal release path.
    static QCoro::Task<void> release(SourceHub *sources, DownloadPlan plan);
    void transfer(const std::shared_ptr<Job>& job, DownloadPlan plan);
    void consume(const std::shared_ptr<Job>& job);
    void finish(const std::shared_ptr<Job>& job, const QString& error = {});
    bool persist();
    void restore();
    static QString defaultDestination();
    SourceHub *m_sources;
    QString m_dataRoot;
    QString m_destination;
    QString m_problem;
    QNetworkAccessManager *m_network;
    AndroidDownloadStorage *m_androidStorage;
    QHash<QString, std::shared_ptr<Job>> m_jobs;
    QStringList m_order;
#if defined(SPOOL_WEBOS)
    bool m_enabled = false;
#else
    bool m_enabled = true;
#endif
    bool m_opened = false;
    QString m_selectionItemId;
};
} // namespace Spool
