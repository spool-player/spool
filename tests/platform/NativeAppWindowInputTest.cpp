#include "platform/NativeAppWindow.h"

#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMouseEvent>

#include <cstdlib>
#include <iostream>

namespace {
using SpoolTests::require;

bool sendMouseButton(
    Spool::NativeAppWindow& window, QEvent::Type type, Qt::MouseButton button, Qt::MouseEventSource source)
{
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonPress ? Qt::MouseButtons(button) : Qt::NoButton;
    QMouseEvent event(type, QPointF(4, 4), QPointF(4, 4), QPointF(4, 4), button, buttons, Qt::NoModifier, source);
    return QCoreApplication::sendEvent(&window, &event) && event.isAccepted();
}
} // namespace

SPOOL_TEST_MAIN("native-window-input")
{
#if !defined(Q_OS_ANDROID) && !defined(SPOOL_APPLE_MOBILE)
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#endif
    QGuiApplication app(argc, argv);
    Spool::NativeAppWindow window(QStringLiteral("input-test"));

    int backRequests = 0;
    int forwardRequests = 0;
    QObject::connect(&window, &Spool::NativeAppWindow::pointerBackRequested, [&backRequests] { ++backRequests; });
    QObject::connect(
        &window, &Spool::NativeAppWindow::pointerForwardRequested, [&forwardRequests] { ++forwardRequests; });

    require(sendMouseButton(window, QEvent::MouseButtonPress, Qt::BackButton, Qt::MouseEventNotSynthesized),
        "physical Back press is consumed");
    require(sendMouseButton(window, QEvent::MouseButtonRelease, Qt::BackButton, Qt::MouseEventNotSynthesized),
        "physical Back release is consumed");
    require(backRequests == 1, "physical Back emits exactly once per click");

    require(sendMouseButton(window, QEvent::MouseButtonPress, Qt::ForwardButton, Qt::MouseEventNotSynthesized),
        "physical Forward press is consumed");
    require(sendMouseButton(window, QEvent::MouseButtonRelease, Qt::ForwardButton, Qt::MouseEventNotSynthesized),
        "physical Forward release is consumed");
    require(forwardRequests == 1, "physical Forward emits exactly once per click");

    require(sendMouseButton(window, QEvent::MouseButtonPress, Qt::BackButton, Qt::MouseEventSynthesizedByQt),
        "synthesized Back press is consumed");
    require(sendMouseButton(window, QEvent::MouseButtonRelease, Qt::BackButton, Qt::MouseEventSynthesizedByQt),
        "synthesized Back release is consumed");
    require(backRequests == 1, "synthesized touch Back does not navigate");

    return 0;
}
