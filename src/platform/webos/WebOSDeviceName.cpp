#include "WebOSDeviceName.h"

#include <QByteArray>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>

extern "C" {
#include <luna-service2/lunaservice.h>
#include <webos-helpers/libhelpers.h>
}

namespace Spool {
namespace {

    std::function<void(const QString&)> g_callback;
    std::unique_ptr<HContext> g_context;

    bool onSystemSettings(LSHandle *, LSMessage *message, void *)
    {
        if (!message || !g_callback)
            return true;

        const QJsonObject payload = QJsonDocument::fromJson(QByteArray(LSMessageGetPayload(message))).object();
        if (!payload.value(QStringLiteral("returnValue")).toBool())
            return true;

        const QString name = payload.value(QStringLiteral("settings"))
                                 .toObject()
                                 .value(QStringLiteral("deviceName"))
                                 .toString()
                                 .trimmed();
        if (name.isEmpty())
            return true;

        qInfo() << "webos: device name is" << name;
        g_callback(name);
        return true;
    }

} // namespace

void requestWebOSDeviceName(std::function<void(const QString&)> callback)
{
    if (!callback)
        return;
    g_callback = std::move(callback);
    g_context = std::make_unique<HContext>();
    g_context->pub = true;
    // Subscribed, so renaming the television in Settings updates the name this
    // client reports without a restart.
    g_context->multiple = true;
    g_context->callback = &onSystemSettings;

    if (HLunaServiceCall("luna://com.webos.settingsservice/getSystemSettings",
            R"({"category":"network","keys":["deviceName"],"subscribe":true})", g_context.get())) {
        qInfo() << "webos: device name unavailable; keeping the platform default";
        g_context.reset();
        g_callback = nullptr;
    }
}

} // namespace Spool
