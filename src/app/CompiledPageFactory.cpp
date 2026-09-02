#include "CompiledPageFactory.h"

#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>

#include "homepage.h"

namespace JellyfinNative {

CompiledPageFactory::CompiledPageFactory(QQmlEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
}

bool CompiledPageFactory::canCreate(const QString& key) const
{
    // One binary, both paths. Comparing a compiled page against an interpreted
    // one by building twice compares two binaries as much as two strategies;
    // this way the only thing that differs between the two runs is which
    // branch the route host takes.
    static const bool disabled = qEnvironmentVariableIsSet("SPOOL_QMLTC_DISABLE");
    if (disabled || !m_engine)
        return false;
    return key == QLatin1String("home");
}

namespace {

    // Builds the same document the slow way, so the two costs are measured in the
    // same process, against the same engine, moments apart. Anything else compares
    // two machines or two builds as much as it compares two strategies.
    qint64 timeInterpretedBuild(QQmlEngine *engine, const QVariant& shell)
    {
        QQmlComponent component(engine, QUrl(QStringLiteral("qrc:/qt/qml/JellyfinWebOS/qml/pages/HomePage.qml")));
        if (component.isError()) {
            qWarning("qmltc compare: interpreted build unavailable: %s", qPrintable(component.errorString()));
            return -1;
        }
        QElapsedTimer timer;
        timer.start();
        std::unique_ptr<QObject> page(component.createWithInitialProperties({ { QStringLiteral("shell"), shell } }));
        const qint64 elapsed = timer.nsecsElapsed();
        return page ? elapsed : -1;
    }

} // namespace

QQuickItem *CompiledPageFactory::create(const QString& key, QQuickItem *parent, const QVariant& shell)
{
    if (!canCreate(key))
        return nullptr;

    const bool compare = qEnvironmentVariableIsSet("SPOOL_QMLTC_COMPARE");
    const qint64 interpretedNs = compare ? timeInterpretedBuild(m_engine, shell) : -1;
    QElapsedTimer timer;
    if (compare)
        timer.start();

    // The generated constructor builds the whole object tree, bindings and all,
    // synchronously. That is the trade the type compiler makes: construction
    // stops being an engine walk a Loader can incubate across frames, and
    // becomes one C++ call that runs to completion.
    auto *page = new JellyfinWebOS::HomePage(m_engine, parent,
        [&shell](JellyfinWebOS::HomePage::PropertyInitializer& properties) { properties.setShell(shell); });
    if (compare) {
        const qint64 compiledNs = timer.nsecsElapsed();
        qInfo("qmltc compare: HomePage construct interpreted %.3f ms compiled %.3f ms (%.1fx)", interpretedNs / 1e6,
            compiledNs / 1e6, compiledNs > 0 ? double(interpretedNs) / compiledNs : 0.0);
    }

    // The generated QML_init evaluates the document's bindings in QML_endInit
    // and only then runs the initializer, so anything binding to shell has
    // already read it once as undefined. Assigning it again afterwards is what
    // makes those bindings re-evaluate against the real value.
    page->setShell(shell);
    page->setParentItem(parent);
    return page;
}

} // namespace JellyfinNative
