#pragma once

#include <QString>

namespace Spool::CredentialStore {

QString load(const QString& profileId);
bool save(const QString& profileId, const QString& accessToken);
void remove(const QString& profileId);
void clear();

} // namespace Spool::CredentialStore
