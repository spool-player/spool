#include "MpvLifecycle.h"
extern "C" {
#include <mpv/client.h>
}

#include <QDebug>

#include <utility>

namespace Spool {

MpvLifecycle::~MpvLifecycle()
{
    destroy();
}

mpv_handle *MpvLifecycle::handle() const
{
    return m_handle.load();
}

bool MpvLifecycle::adopt(mpv_handle *handle, EventHandler eventHandler)
{
    if (!handle || m_handle.load() || m_eventThread.joinable())
        return false;

    m_pendingFileLoads = 0;
    m_handle = handle;
    m_stopFlag = std::make_shared<std::atomic_bool>(false);
    m_eventThread = std::thread([handle, eventHandler = std::move(eventHandler), stop = m_stopFlag]() mutable {
        runEventLoop(handle, std::move(eventHandler), stop);
    });
    return true;
}

void MpvLifecycle::destroy(BeforeDestroy beforeDestroy)
{
    mpv_handle *handle = m_handle.exchange(nullptr);
    if (!handle) {
        if (m_eventThread.joinable())
            m_eventThread.join();
        return;
    }

    if (beforeDestroy)
        beforeDestroy(handle);

    if (m_stopFlag)
        m_stopFlag->store(true);
    mpv_wakeup(handle);
    if (m_eventThread.joinable())
        m_eventThread.join();

    qInfo() << "player: calling mpv_terminate_destroy";
    mpv_terminate_destroy(handle);
    qInfo() << "player: mpv_terminate_destroy returned";

    m_stopFlag.reset();
    m_pendingFileLoads = 0;
}

void MpvLifecycle::destroyAsync(BeforeDestroy beforeDestroy)
{
    mpv_handle *handle = m_handle.exchange(nullptr);
    if (!handle) {
        if (m_eventThread.joinable())
            m_eventThread.join();
        return;
    }

    if (beforeDestroy)
        beforeDestroy(handle);

    if (m_stopFlag)
        m_stopFlag->store(true);
    mpv_wakeup(handle);
    if (m_eventThread.joinable())
        m_eventThread.join();
    m_stopFlag.reset();
    m_pendingFileLoads = 0;

    // The event handler can no longer run; only the handle outlives this object.
    std::thread([handle]() {
        qInfo() << "player: calling mpv_terminate_destroy (async)";
        mpv_terminate_destroy(handle);
        qInfo() << "player: mpv_terminate_destroy returned";
    }).detach();
}

void MpvLifecycle::beginFileLoad()
{
    m_pendingFileLoads.fetch_add(1, std::memory_order_acq_rel);
}

void MpvLifecycle::cancelFileLoad()
{
    int pending = m_pendingFileLoads.load(std::memory_order_acquire);
    while (pending > 0
        && !m_pendingFileLoads.compare_exchange_weak(
            pending, pending - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

void MpvLifecycle::completeFileLoad()
{
    cancelFileLoad();
}

bool MpvLifecycle::hasPendingFileLoads() const
{
    return m_pendingFileLoads.load(std::memory_order_acquire) > 0;
}

void MpvLifecycle::runEventLoop(
    mpv_handle *handle, EventHandler eventHandler, const std::shared_ptr<std::atomic_bool>& stop)
{
    while (!stop->load()) {
        mpv_event *event = mpv_wait_event(handle, 0.1);
        if (stop->load())
            break;
        if (event && event->event_id == MPV_EVENT_SHUTDOWN)
            stop->store(true);
        if (event && eventHandler)
            eventHandler(event);
    }
}

} // namespace Spool
