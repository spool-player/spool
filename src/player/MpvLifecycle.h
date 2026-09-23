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
    // Hands the event-thread join and mpv_terminate_destroy to a detached
    // worker: the join can wait out a full mpv_wait_event timeout and the
    // destroy tears down the demuxer, neither of which belongs on the GUI
    // thread. Safe to call from the GUI thread mid-session.
    void destroyAsync(BeforeDestroy beforeDestroy = {});
    void requestEventLoopStop();

    void beginFileLoad();
    void cancelFileLoad();
    void completeFileLoad();
    bool hasPendingFileLoads() const;

private:
    static void runEventLoop(
        mpv_handle *handle, EventHandler eventHandler, const std::shared_ptr<std::atomic_bool>& stop);

    std::thread m_eventThread;
    // Owned per adoption and shared with the event thread so a detached
    // teardown worker never needs to touch `this`.
    std::shared_ptr<std::atomic_bool> m_stopFlag;
    std::atomic<int> m_pendingFileLoads { 0 };
    std::atomic<mpv_handle *> m_handle { nullptr };
};

} // namespace Spool
