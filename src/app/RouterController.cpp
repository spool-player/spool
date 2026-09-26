#include "RouterController.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QSet>
#include <QSettings>

namespace Spool {

namespace {

    constexpr auto kCleanShutdownKey = "uiSession/cleanShutdown";
    constexpr auto kRestoreNextLaunchKey = "uiSession/restoreNextLaunch";
    constexpr auto kRestoreReasonKey = "uiSession/restoreReason";
    constexpr auto kSnapshotKey = "uiSession/snapshot";

    bool recoverableRoute(const QString& route)
    {
        static const QSet<QString> routes = { QStringLiteral("home"), QStringLiteral("libraryGrid"),
            QStringLiteral("itemDetails"), QStringLiteral("personDetails"), QStringLiteral("search"),
            QStringLiteral("settings"), QStringLiteral("subtitleSettings"), QStringLiteral("openSourceNotices") };
        return routes.contains(route);
    }

    QVariantMap recoverableArgs(const QVariantMap& args)
    {
        static const QSet<QString> keys = { QStringLiteral("itemId"), QStringLiteral("itemType"),
            QStringLiteral("source"), QStringLiteral("returnRoute"), QStringLiteral("focusIndex"),
            QStringLiteral("libraryId"), QStringLiteral("personId"), QStringLiteral("personName"),
            QStringLiteral("personRole"), QStringLiteral("personType"), QStringLiteral("title"),
            QStringLiteral("seriesId"), QStringLiteral("seasonId") };
        QVariantMap safe;
        for (auto it = args.cbegin(); it != args.cend(); ++it) {
            if (!keys.contains(it.key()))
                continue;
            const QVariant& value = it.value();
            if (value.metaType().id() == QMetaType::Bool || value.canConvert<qlonglong>()
                || value.metaType().id() == QMetaType::QString) {
                safe.insert(it.key(), value);
            }
        }
        return safe;
    }

    QVariantMap recoverableFrame(const QVariantMap& value)
    {
        const QString route = value.value(QStringLiteral("route")).toString();
        if (!recoverableRoute(route))
            return {};
        return { { QStringLiteral("route"), route },
            { QStringLiteral("args"), recoverableArgs(value.value(QStringLiteral("args")).toMap()) } };
    }

} // namespace

RouterController::RouterController(QObject *parent)
    : QObject(parent)
{
}

RouterController::RouterController(const QString& startRoute, QObject *parent)
    : QObject(parent)
    , m_route(startRoute)
{
}

QString RouterController::route() const
{
    return m_route;
}

QString RouterController::previousRoute() const
{
    return m_previousRoute;
}

QVariantMap RouterController::args() const
{
    return m_args;
}

QVariantList RouterController::stack() const
{
    return m_stack;
}

bool RouterController::canPop() const
{
    return !m_stack.isEmpty();
}
bool RouterController::canForward() const
{
    return !m_forwardStack.isEmpty();
}

bool RouterController::recoveryPending() const
{
    return m_recoveryPending;
}

void RouterController::beginSession(bool allowRecovery)
{
    if (m_sessionStarted)
        return;
    m_sessionStarted = true;

    QSettings settings;
    const bool previousClean = settings.value(QLatin1String(kCleanShutdownKey), true).toBool();
    const bool explicitRestore = settings.value(QLatin1String(kRestoreNextLaunchKey), false).toBool();
    if (allowRecovery && (!previousClean || explicitRestore)) {
        m_recoveryPending = restoreSnapshot();
        if (m_recoveryPending)
            emit recoveryPendingChanged();
    }

    settings.setValue(QLatin1String(kCleanShutdownKey), false);
    settings.sync();
}

void RouterController::requestRecoveryOnNextLaunch(const QString& reason)
{
    m_recoveryRequested = true;
    persistSnapshot();
    QSettings settings;
    settings.setValue(QLatin1String(kRestoreNextLaunchKey), true);
    settings.setValue(QLatin1String(kRestoreReasonKey), reason);
    settings.sync();
}

void RouterController::markCleanShutdown()
{
    if (!m_sessionStarted)
        return;
    persistSnapshot();
    QSettings settings;
    settings.setValue(QLatin1String(kCleanShutdownKey), true);
    if (!m_recoveryRequested) {
        settings.setValue(QLatin1String(kRestoreNextLaunchKey), false);
        settings.remove(QLatin1String(kRestoreReasonKey));
    }
    settings.sync();
}

QVariantMap RouterController::frame(const QString& route, const QVariantMap& args) const
{
    return { { QStringLiteral("route"), route }, { QStringLiteral("args"), args } };
}

void RouterController::setFrame(const QString& route, const QVariantMap& args, const QString& previousRoute)
{
    const bool routeChanged = route != m_route || args != m_args || previousRoute != m_previousRoute;
    if (!routeChanged)
        return;
    m_previousRoute = previousRoute;
    m_route = route.isEmpty() ? QStringLiteral("home") : route;
    m_args = args;
    emit this->routeChanged();
    persistSnapshot();
}

void RouterController::reset(const QString& route, const QVariantMap& args)
{
    const bool historyChanged = !m_stack.isEmpty() || !m_forwardStack.isEmpty();
    m_stack.clear();
    m_forwardStack.clear();
    setFrame(route, args, QStringLiteral("home"));
    if (historyChanged)
        emit stackChanged();
    persistSnapshot();
}

void RouterController::push(const QString& route, const QVariantMap& args)
{
    if (route.isEmpty() || (route == m_route && args == m_args))
        return;
    const QString previous = m_route;
    m_stack.push_back(frame(m_route, m_args));
    m_forwardStack.clear();
    emit stackChanged();
    setFrame(route, args, previous);
}

void RouterController::replace(const QString& route, const QVariantMap& args)
{
    if (route.isEmpty())
        return;
    const bool hadForwardHistory = !m_forwardStack.isEmpty();
    m_forwardStack.clear();
    setFrame(route, args, m_route);
    if (hadForwardHistory)
        emit stackChanged();
}

bool RouterController::pop(const QString& fallbackRoute)
{
    if (m_stack.isEmpty()) {
        replace(fallbackRoute.isEmpty() ? QStringLiteral("home") : fallbackRoute);
        return false;
    }
    const QVariantMap top = m_stack.takeLast().toMap();
    m_forwardStack.push_back(frame(m_route, m_args));
    emit stackChanged();
    setFrame(top.value(QStringLiteral("route")).toString(), top.value(QStringLiteral("args")).toMap(), m_route);
    return true;
}

bool RouterController::forward()
{
    if (m_forwardStack.isEmpty())
        return false;
    const QVariantMap next = m_forwardStack.takeLast().toMap();
    m_stack.push_back(frame(m_route, m_args));
    emit stackChanged();
    setFrame(next.value(QStringLiteral("route")).toString(), next.value(QStringLiteral("args")).toMap(), m_route);
    return true;
}

void RouterController::checkpoint(const QVariantMap& args)
{
    const QVariantMap safe = recoverableArgs(args);
    for (auto it = safe.cbegin(); it != safe.cend(); ++it)
        m_args.insert(it.key(), it.value());
    persistSnapshot();
}

void RouterController::finishRecovery()
{
    if (!m_recoveryPending)
        return;
    m_recoveryPending = false;
    emit recoveryPendingChanged();
    QSettings settings;
    settings.setValue(QLatin1String(kRestoreNextLaunchKey), false);
    settings.remove(QLatin1String(kRestoreReasonKey));
    settings.sync();
}

void RouterController::persistSnapshot() const
{
    if (!m_sessionStarted || !recoverableRoute(m_route))
        return;

    QVariantList safeStack;
    safeStack.reserve(m_stack.size());
    for (const QVariant& value : m_stack) {
        const QVariantMap safe = recoverableFrame(value.toMap());
        if (!safe.isEmpty())
            safeStack.push_back(safe);
    }
    const QVariantMap snapshot = { { QStringLiteral("schema"), 1 }, { QStringLiteral("route"), m_route },
        { QStringLiteral("previousRoute"),
            recoverableRoute(m_previousRoute) ? m_previousRoute : QStringLiteral("home") },
        { QStringLiteral("args"), recoverableArgs(m_args) }, { QStringLiteral("stack"), safeStack } };
    QSettings settings;
    settings.setValue(QLatin1String(kSnapshotKey), QJsonDocument::fromVariant(snapshot).toJson(QJsonDocument::Compact));
}

bool RouterController::restoreSnapshot()
{
    const QByteArray encoded = QSettings().value(QLatin1String(kSnapshotKey)).toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(encoded);
    if (!document.isObject())
        return false;
    const QVariantMap snapshot = document.object().toVariantMap();
    if (snapshot.value(QStringLiteral("schema")).toInt() != 1)
        return false;
    const QString route = snapshot.value(QStringLiteral("route")).toString();
    if (!recoverableRoute(route))
        return false;

    QVariantList safeStack;
    for (const QVariant& value : snapshot.value(QStringLiteral("stack")).toList()) {
        const QVariantMap safe = recoverableFrame(value.toMap());
        if (!safe.isEmpty())
            safeStack.push_back(safe);
    }
    m_route = route;
    m_previousRoute = snapshot.value(QStringLiteral("previousRoute"), QStringLiteral("home")).toString();
    if (!recoverableRoute(m_previousRoute))
        m_previousRoute = QStringLiteral("home");
    m_args = recoverableArgs(snapshot.value(QStringLiteral("args")).toMap());
    m_stack = std::move(safeStack);
    m_forwardStack.clear();
    return true;
}

} // namespace Spool
