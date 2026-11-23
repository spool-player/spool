#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

struct mpv_event;
struct mpv_handle;

namespace Spool {

class MpvLifecycle final {
public:
    using EventHandler = std::function<void(mpv_event *)>;
    using BeforeDestroy = std::function<void(mpv_handle *)>;

    ~MpvLifecycle();

    mpv_handle *handle() const;
    bool adopt(mpv_handle *handle, EventHandler eventHandler);
    void destroy(BeforeDestroy beforeDestroy = {});
    // Stops event delivery before returning, then hands mpv_terminate_destroy
    // to a detached worker. Event handlers may refer to the owning controller;
    // only destruction of the self-contained mpv core may outlive it.
    void destroyAsync(BeforeDestroy beforeDestroy = {});

    void beginFileLoad();
    void cancelFileLoad();
    void completeFileLoad();
    bool hasPendingFileLoads() const;

private:
    static void runEventLoop(
        mpv_handle *handle, EventHandler eventHandler, const std::shared_ptr<std::atomic_bool>& stop);

    std::thread m_eventThread;
    // Each adopted core has its own stop flag, shared with its event thread.
    std::shared_ptr<std::atomic_bool> m_stopFlag;
    std::atomic<int> m_pendingFileLoads { 0 };
    std::atomic<mpv_handle *> m_handle { nullptr };
};

} // namespace Spool
