#pragma once

#include <cstdint>
#include <vector>

namespace Spool {

struct GroupClockSample {
    std::int64_t localRequestSentMs = 0;
    std::int64_t serverRequestReceivedMs = 0;
    std::int64_t serverResponseSentMs = 0;
    std::int64_t localResponseReceivedMs = 0;

    double offsetMs() const;
    double roundTripDelayMs() const;
    double pingMs() const;
};

class GroupClock final {
public:
    void reset();
    void addMeasurement(const GroupClockSample& measurement);

    bool ready() const;
    double offsetMs() const;
    double pingMs() const;
    std::int64_t localDelayUntil(std::int64_t serverTimeMs, std::int64_t localNowMs) const;
    std::int64_t estimatePositionTicks(
        std::int64_t positionTicks, std::int64_t serverTimeMs, std::int64_t localNowMs) const;

private:
    void selectBestMeasurement();

    std::vector<GroupClockSample> m_measurements;
    GroupClockSample m_best;
    bool m_ready = false;
};

} // namespace Spool
