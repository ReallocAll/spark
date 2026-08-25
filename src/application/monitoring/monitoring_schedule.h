#ifndef SPARK_APPLICATION_MONITORING_MONITORING_SCHEDULE_H
#define SPARK_APPLICATION_MONITORING_MONITORING_SCHEDULE_H

#include <cstdint>

namespace spark {

struct MonitoringDue {
    bool ping = false;
    bool network = false;
};

class MonitoringSchedule {
public:
    static constexpr std::int64_t kPingIntervalMs = 10'000;
    static constexpr std::int64_t kNetworkIntervalMs = 60'000;

    MonitoringSchedule();
    explicit MonitoringSchedule(std::int64_t now_ms);

    MonitoringDue poll(std::int64_t now_ms);

private:
    static bool initializeDeadline(std::int64_t now_ms, std::int64_t interval_ms, std::int64_t &deadline_ms);
    static bool pollDeadline(std::int64_t now_ms, std::int64_t interval_ms, std::int64_t &deadline_ms, bool &enabled);

    std::int64_t next_ping_ms_ = 0;
    std::int64_t next_network_ms_ = 0;
    bool ping_enabled_ = true;
    bool network_enabled_ = true;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_MONITORING_MONITORING_SCHEDULE_H
