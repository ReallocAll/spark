#include "application/monitoring/monitoring_schedule.h"

#include <cstdint>
#include <limits>

#include "core/util/monotonic_time.h"

namespace spark {

MonitoringSchedule::MonitoringSchedule() : MonitoringSchedule(monotonicUnixMillis()) {}

MonitoringSchedule::MonitoringSchedule(std::int64_t now_ms)
{
    ping_enabled_ = initializeDeadline(now_ms, kPingIntervalMs, next_ping_ms_);
    network_enabled_ = initializeDeadline(now_ms, kNetworkIntervalMs, next_network_ms_);
}

MonitoringDue MonitoringSchedule::poll(std::int64_t now_ms)
{
    return {.ping = pollDeadline(now_ms, kPingIntervalMs, next_ping_ms_, ping_enabled_),
            .network = pollDeadline(now_ms, kNetworkIntervalMs, next_network_ms_, network_enabled_)};
}

bool MonitoringSchedule::initializeDeadline(std::int64_t now_ms, std::int64_t interval_ms, std::int64_t &deadline_ms)
{
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();
    if (now_ms > k_max - interval_ms) {
        return false;
    }
    deadline_ms = now_ms + interval_ms;
    return true;
}

bool MonitoringSchedule::pollDeadline(std::int64_t now_ms, std::int64_t interval_ms, std::int64_t &deadline_ms,
                                      bool &enabled)
{
    if (!enabled || now_ms < deadline_ms) {
        return false;
    }

    const auto elapsed = static_cast<std::uint64_t>(now_ms) - static_cast<std::uint64_t>(deadline_ms);
    const auto intervals = elapsed / static_cast<std::uint64_t>(interval_ms) + 1;
    const auto remaining =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - static_cast<std::uint64_t>(deadline_ms);
    if (intervals > remaining / static_cast<std::uint64_t>(interval_ms)) {
        enabled = false;
    }
    else {
        const auto advance = intervals * static_cast<std::uint64_t>(interval_ms);
        deadline_ms = static_cast<std::int64_t>(static_cast<std::uint64_t>(deadline_ms) + advance);
    }
    return true;
}

}  // namespace spark
