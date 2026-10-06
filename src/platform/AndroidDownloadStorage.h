#pragma once

#include <QObject>
#include <QUrl>

#include <memory>

namespace Spool {

class AndroidDownloadStorage final : public QObject {
    Q_OBJECT

public:
    explicit AndroidDownloadStorage(QObject *parent = nullptr);
    ~AndroidDownloadStorage() override;

    Q_INVOKABLE void chooseFolder();

    static bool isTree(const QUrl& url);
    static QUrl createFile(const QUrl& tree, const QString& name);
    // The returned URI can differ from the original. Close the file before renaming.
    static QUrl renameFile(const QUrl& document, const QString& name);
    static bool removeFile(const QUrl& document);

signals:
    void folderSelected(const QUrl& tree);
    void problem(const QString& message);

private:
#ifdef Q_OS_ANDROID
    class ResultReceiver;
    std::unique_ptr<ResultReceiver> m_receiver;
    bool m_pickerOpen = false;
#endif
};

} // namespace Spool
