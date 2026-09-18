#pragma once

#include "../media/MediaTypes.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <vector>

namespace JellyfinNative {

class BrowseSessionController;
class JellyfinApiFacade;

class LibraryManagementController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool currentUserCanManagePlaylists MEMBER m_currentUserCanManagePlaylists NOTIFY policyChanged)
    Q_PROPERTY(bool currentUserCanManageCollections MEMBER m_currentUserCanManageCollections NOTIFY policyChanged)
    Q_PROPERTY(bool currentUserCanRenameItems MEMBER m_currentUserCanRenameItems NOTIFY policyChanged)
    Q_PROPERTY(bool currentUserCanDeleteItems MEMBER m_currentUserCanDeleteItems NOTIFY policyChanged)
    Q_PROPERTY(QVariantList playlistTargets MEMBER m_playlistTargets NOTIFY targetsChanged)
    Q_PROPERTY(QVariantList collectionTargets MEMBER m_collectionTargets NOTIFY targetsChanged)

public:
    LibraryManagementController(JellyfinApiFacade *api, BrowseSessionController *browse, QObject *parent = nullptr);

    Q_INVOKABLE void clear();
    Q_INVOKABLE void loadCurrentUserPolicy();
    Q_INVOKABLE void refreshTargets(const QString& kind);
    Q_INVOKABLE void createPlaylistForItem(const QString& name, const MovieItem& item);
    Q_INVOKABLE void addItemToPlaylist(const QString& playlistId, const MovieItem& item);
    Q_INVOKABLE void createCollectionForItem(const QString& name, const MovieItem& item);
    Q_INVOKABLE void addItemToCollection(const QString& collectionId, const MovieItem& item);
    Q_INVOKABLE void removeItemFromCurrentParent(const MovieItem& item);
    Q_INVOKABLE void movePlaylistItemInCurrent(const MovieItem& item, int delta);
    Q_INVOKABLE void renameItem(const MovieItem& item, const QString& name);
    Q_INVOKABLE void deleteItem(const MovieItem& item);

signals:
    void policyChanged();
    void targetsChanged();
    void errorOccurred(const QString& errorText);
    void operationSucceeded(const QString& action);
    void refreshRequested(const QString& changedItemId);

private:
    void setTargets(const QString& kind, const std::vector<MovieItem>& items);
    void refreshAfterMutation(const QString& changedItemId = {});
    QStringList itemIdsFor(const MovieItem& item) const;
    bool authenticated();
    bool playlistAllowed();
    bool collectionAllowed();
    bool renameAllowed(const QString& itemType);
    bool deleteAllowed();

    JellyfinApiFacade *m_api = nullptr;
    BrowseSessionController *m_browse = nullptr;
    QVariantList m_playlistTargets;
    QVariantList m_collectionTargets;
    bool m_policyLoadStarted = false;
    bool m_currentUserCanManagePlaylists = false;
    bool m_currentUserCanManageCollections = false;
    bool m_currentUserCanRenameItems = false;
    bool m_currentUserCanDeleteItems = false;
};

} // namespace JellyfinNative
