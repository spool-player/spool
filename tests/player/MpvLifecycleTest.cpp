#include "player/MpvLifecycle.h"

#include <mpv/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

using namespace Spool;
using namespace std::chrono_literals;

// This target links the real MpvLifecycle with these three fake client API
// functions, not libmpv. The fake exposes the races a real idle core cannot
// reliably schedule for a test: wakeup just before waiting, a busy callback,
// and asynchronous teardown overlapping another adoption.
struct mpv_handle {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<mpv_event_id> pending;
    mpv_event event {};
    bool wakePending = false;
    bool holdWaitEntry = false;
    int waitCalls = 0;
    int timedWaits = 0;
    int wakeups = 0;
    int destroyed = 0;
};

mpv_event *mpv_wait_event(mpv_handle *handle, double timeout)
{
    std::unique_lock lock(handle->mutex);
    ++handle->waitCalls;
    if (timeout >= 0)
        ++handle->timedWaits;
    handle->changed.notify_all();
    handle->changed.wait(lock, [&] { return !handle->holdWaitEntry; });
    const auto ready = [&] { return handle->wakePending || !handle->pending.empty(); };
    if (timeout < 0)
        handle->changed.wait(lock, ready);
    else
        handle->changed.wait_for(lock, std::chrono::duration<double>(timeout), ready);
    handle->event = {};
    if (!handle->pending.empty()) {
        handle->event.event_id = handle->pending.front();
        handle->pending.pop_front();
    }
    handle->wakePending = false;
    return &handle->event;
}

void mpv_wakeup(mpv_handle *handle)
{
    std::lock_guard lock(handle->mutex);
    ++handle->wakeups;
    handle->wakePending = true;
    handle->changed.notify_all();
}

void mpv_terminate_destroy(mpv_handle *handle)
{
    std::lock_guard lock(handle->mutex);
    ++handle->destroyed;
    handle->changed.notify_all();
}

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void waitFor(mpv_handle& handle, Predicate predicate, const char *message)
{
    std::unique_lock lock(handle.mutex);
    require(handle.changed.wait_for(lock, 2s, predicate), message);
}

void enqueue(mpv_handle& handle, mpv_event_id event)
{
    std::lock_guard lock(handle.mutex);
    handle.pending.push_back(event);
    handle.changed.notify_all();
}

void idleWaitAndSynchronousDestroy()
{
    mpv_handle handle;
    MpvLifecycle lifecycle;
    std::atomic_int events { 0 };
    require(!lifecycle.adopt(nullptr, {}), "a null handle must be rejected");
    require(lifecycle.adopt(&handle, [&](mpv_event *) { ++events; }), "adoption failed");
    require(!lifecycle.adopt(&handle, {}), "an active lifecycle must reject a second adoption");
    waitFor(handle, [&] { return handle.waitCalls == 1; }, "event loop did not start");
    {
        std::unique_lock lock(handle.mutex);
        require(!handle.changed.wait_for(lock, 250ms, [&] { return handle.waitCalls > 1; }),
            "an idle player must not repeatedly wake to poll");
        require(handle.timedWaits == 0, "event loop must use an interruptible indefinite wait");
    }
    enqueue(handle, MPV_EVENT_CLIENT_MESSAGE);
    enqueue(handle, MPV_EVENT_PROPERTY_CHANGE);
    waitFor(handle, [&] { return handle.waitCalls >= 3; }, "queued events were not drained");
    require(events == 2, "real events must reach the handler exactly once");
    mpv_wakeup(&handle);
    waitFor(handle, [&] { return handle.waitCalls >= 4; }, "a spurious wakeup stalled the event loop");
    require(events == 2, "a wakeup without an event must not reach the handler");
    int detachCalls = 0;
    lifecycle.destroy([&](mpv_handle *detached) {
        require(detached == &handle && !lifecycle.handle(), "detach must receive the withdrawn handle");
        ++detachCalls;
    });
    require(!lifecycle.handle() && handle.destroyed == 1 && detachCalls == 1, "synchronous teardown failed");
    require(events == 2, "shutdown wakeup must not dispatch a callback");
    lifecycle.destroy();
    require(handle.destroyed == 1, "repeated teardown must not destroy the core again");
}

void wakeBeforeWait()
{
    mpv_handle handle;
    handle.holdWaitEntry = true;
    MpvLifecycle lifecycle;
    std::atomic_int events { 0 };
    require(lifecycle.adopt(&handle, [&](mpv_event *) { ++events; }), "adoption failed");
    waitFor(handle, [&] { return handle.waitCalls == 1; }, "event loop did not enter the wait");
    std::thread destroyer([&] { lifecycle.destroy(); });
    waitFor(handle, [&] { return handle.wakeups == 1; }, "teardown did not wake the event loop");
    {
        std::lock_guard lock(handle.mutex);
        handle.holdWaitEntry = false;
        handle.changed.notify_all();
    }
    waitFor(handle, [&] { return handle.destroyed == 1; }, "a wakeup before the wait was lost");
    destroyer.join();
    require(events == 0, "a stopped loop must not dispatch the wakeup event");
}

void shutdownEventStopsItsOwnLoop()
{
    mpv_handle handle;
    std::atomic_int events { 0 };
    MpvLifecycle lifecycle;
    require(lifecycle.adopt(&handle,
                [&](mpv_event *event) {
                    require(event->event_id == MPV_EVENT_SHUTDOWN, "unexpected shutdown event");
                    ++events;
                }),
        "adoption failed");
    waitFor(handle, [&] { return handle.waitCalls == 1; }, "event loop did not start");
    enqueue(handle, MPV_EVENT_SHUTDOWN);
    for (int attempt = 0; events == 0 && attempt < 200; ++attempt)
        std::this_thread::sleep_for(1ms);
    require(events == 1, "shutdown was not dispatched");
    {
        std::unique_lock lock(handle.mutex);
        require(!handle.changed.wait_for(lock, 50ms, [&] { return handle.waitCalls > 1; }),
            "shutdown must stop without consulting a later adoption's state");
    }
    lifecycle.destroy();
    require(handle.destroyed == 1, "shutdown must still release its mpv core");
}

void asynchronousDestroyJoinsCallbacksThenAllowsReuse()
{
    mpv_handle oldHandle;
    mpv_handle newHandle;
    std::mutex callbackMutex;
    std::condition_variable callbackChanged;
    bool callbackStarted = false;
    bool releaseCallback = false;
    {
        MpvLifecycle lifecycle;
        require(lifecycle.adopt(&oldHandle,
                    [&](mpv_event *) {
                        std::unique_lock lock(callbackMutex);
                        callbackStarted = true;
                        callbackChanged.notify_all();
                        callbackChanged.wait(lock, [&] { return releaseCallback; });
                    }),
            "old adoption failed");
        enqueue(oldHandle, MPV_EVENT_CLIENT_MESSAGE);
        {
            std::unique_lock lock(callbackMutex);
            require(callbackChanged.wait_for(lock, 2s, [&] { return callbackStarted; }), "callback did not start");
        }
        std::thread destroyer([&] { lifecycle.destroyAsync(); });
        // The handle is withdrawn before the callback join begins.
        for (int attempt = 0; lifecycle.handle() && attempt < 2000; ++attempt)
            std::this_thread::sleep_for(1ms);
        require(!lifecycle.handle(), "async teardown must withdraw its handle");
        waitFor(oldHandle, [&] { return oldHandle.wakeups >= 1; }, "teardown did not wake the event loop");
        {
            std::lock_guard lock(callbackMutex);
            releaseCallback = true;
            callbackChanged.notify_all();
        }
        // The event thread's callbacks are joined before the core's
        // destruction is handed to a worker; destroyAsync returns with them.
        destroyer.join();
        require(!lifecycle.handle(), "async teardown kept its handle");
        require(lifecycle.adopt(&newHandle, {}), "reuse after joined async teardown");
        waitFor(newHandle, [&] { return newHandle.waitCalls == 1; }, "new event loop did not start");
        // Destructor joins and destroys the new core while the old worker
        // may still own the detached terminate_destroy of the old core.
    }
    require(newHandle.destroyed == 1, "destructor failed to release the new core");
    waitFor(oldHandle, [&] { return oldHandle.destroyed == 1; }, "old async teardown did not finish");
    require(oldHandle.waitCalls == 1, "old event loop resumed after lifecycle reuse");
}

} // namespace

int main()
{
    idleWaitAndSynchronousDestroy();
    wakeBeforeWait();
    shutdownEventStopsItsOwnLoop();
    asynchronousDestroyJoinsCallbacksThenAllowsReuse();
    return EXIT_SUCCESS;
}
