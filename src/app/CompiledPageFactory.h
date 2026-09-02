#pragma once

#include <QObject>
#include <QQuickItem>
#include <QString>
#include <QVariant>

#include <memory>

class QQmlEngine;

namespace JellyfinNative {

// Builds pages the QML type compiler turned into C++ classes.
//
// Turning qmltc on is not by itself enough to change anything: it emits a C++
// class per document, and those classes only run if something constructs them.
// The route host builds pages by URL through Loader.setSource(), which makes
// the engine walk the compiled bytecode and build the object tree the slow way
// -- the generated class sits in the binary and is never reached. This is the
// seam where a page is asked for as C++ instead.
//
// Anything not compiled falls through: canCreate() answers false and the route
// host keeps its Loader path, so the two coexist while pages are converted one
// at a time.
class CompiledPageFactory final : public QObject {
    Q_OBJECT

public:
    explicit CompiledPageFactory(QQmlEngine *engine, QObject *parent = nullptr);

    Q_INVOKABLE bool canCreate(const QString& key) const;
    // shell is passed in rather than assigned afterwards: a generated
    // constructor evaluates every binding in the document before it returns,
    // so a property set after the call has already been read as undefined by
    // everything that depends on it. Loader.setSource()'s initial-properties
    // argument is the equivalent, and qmltc exposes it as PropertyInitializer.
    Q_INVOKABLE QQuickItem *create(const QString& key, QQuickItem *parent, const QVariant& shell);

private:
    QQmlEngine *m_engine = nullptr;
};

} // namespace JellyfinNative
