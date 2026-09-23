#pragma once

#include <QByteArray>

#include <memory>

namespace Spool {

class ScreenSaverBackend {
public:
    virtual ~ScreenSaverBackend() = default;
    virtual bool acquire() = 0;
    virtual bool release() = 0;
};

std::unique_ptr<ScreenSaverBackend> createPlatformScreenSaverBackend();

class ScreenSaverInhibitor final {
public:
    ScreenSaverInhibitor();
    explicit ScreenSaverInhibitor(std::unique_ptr<ScreenSaverBackend> backend);
    ~ScreenSaverInhibitor();

    ScreenSaverInhibitor(const ScreenSaverInhibitor&) = delete;
    ScreenSaverInhibitor& operator=(const ScreenSaverInhibitor&) = delete;

    void setInhibited(bool inhibited);
    bool inhibited() const;

private:
    std::unique_ptr<ScreenSaverBackend> m_backend;
    bool m_inhibited = false;
};

bool screenSaverShouldBeInhibited(bool mediaSessionActive, bool paused, bool slideshowAdvancing = false);
QByteArray webOsScreenSaverResponsePayload(const QByteArray& requestPayload);

} // namespace Spool
