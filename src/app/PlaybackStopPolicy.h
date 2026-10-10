#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace Spool {

// One local, process-lifetime stop request. The host supplies monotonic time so
// pauses, item transitions, and a delayed event loop never restart a duration.
class PlaybackStopPolicy final {
public:
    enum class Mode { Off, AfterCurrent, Timed };

    Mode mode() const
    {
        return m_mode;
    }
    int durationMinutes() const
    {
        return m_durationMinutes;
    }

    int remainingSeconds(std::int64_t nowMs) const
    {
        if (m_mode != Mode::Timed)
            return 0;
        return static_cast<int>((std::max<std::int64_t>(0, m_deadlineMs - nowMs) + 999) / 1000);
    }

    bool setDuration(int minutes, std::int64_t nowMs)
    {
        if (minutes < 1 || minutes > 120)
            return false;
        cancel();
        m_mode = Mode::Timed;
        m_durationMinutes = minutes;
        m_deadlineMs = nowMs + static_cast<std::int64_t>(minutes) * 60'000;
        return true;
    }

    bool setAfterCurrent(const std::string& itemId)
    {
        if (itemId.empty())
            return false;
        cancel();
        m_mode = Mode::AfterCurrent;
        m_itemId = itemId;
        return true;
    }

    // Restarting the same item for quality/track changes keeps the request.
    // Deliberately starting another item cancels only after-current mode.
    bool playbackStarting(const std::string& itemId)
    {
        return m_mode == Mode::AfterCurrent && itemId != m_itemId && cancel();
    }

    bool takeCompleted(const std::string& itemId)
    {
        return m_mode == Mode::AfterCurrent && itemId == m_itemId && cancel();
    }

    bool takeExpired(std::int64_t nowMs)
    {
        return m_mode == Mode::Timed && nowMs >= m_deadlineMs && cancel();
    }

    // Used for explicit stops, group joins, logout/profile changes, and exit.
    bool cancel()
    {
        const bool wasActive = m_mode != Mode::Off;
        m_mode = Mode::Off;
        m_durationMinutes = 0;
        m_deadlineMs = 0;
        m_itemId.clear();
        return wasActive;
    }

private:
    Mode m_mode = Mode::Off;
    int m_durationMinutes = 0;
    std::int64_t m_deadlineMs = 0;
    std::string m_itemId;
};

} // namespace Spool
