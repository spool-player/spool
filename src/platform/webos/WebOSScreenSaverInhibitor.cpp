#include "platform/ScreenSaverInhibitor.h"

#include <QDebug>

extern "C" {
#include <luna-service2/lunaservice.h>
#include <webos-helpers/libhelpers.h>
}

namespace Spool {
namespace {

    class WebOSScreenSaverBackend final : public ScreenSaverBackend {
    public:
        bool acquire() override
        {
            auto context = std::make_unique<HContext>();
            context->pub = true;
            context->multiple = true;
            context->callback = &WebOSScreenSaverBackend::request;
            context->userdata = this;
            if (HLunaServiceCall("luna://com.webos.service.tvpower/power/registerScreenSaverRequest",
                    "{\"subscribe\":true,\"clientName\":\"com.sachk.spool\"}", context.get())) {
                qWarning() << "screensaver: webOS inhibit registration failed";
                return false;
            }
            m_subscription = std::move(context);
            return true;
        }

        bool release() override
        {
            if (m_subscription) {
                HUnregisterServiceCallback(m_subscription.get());
                m_subscription.reset();
            }
            return true;
        }

    private:
        static bool request(LSHandle *, LSMessage *message, void *)
        {
            const QByteArray payload
                = webOsScreenSaverResponsePayload(QByteArray(message ? LSMessageGetPayload(message) : ""));
            if (payload.isEmpty())
                return true;
            HContext context {};
            context.pub = true;
            context.multiple = false;
            return HLunaServiceCall("luna://com.webos.service.tvpower/power/responseScreenSaverRequest",
                       payload.constData(), &context)
                == 0;
        }

        std::unique_ptr<HContext> m_subscription;
    };

} // namespace

std::unique_ptr<ScreenSaverBackend> createPlatformScreenSaverBackend()
{
    return std::make_unique<WebOSScreenSaverBackend>();
}

} // namespace Spool
