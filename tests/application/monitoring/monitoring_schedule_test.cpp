#include <cassert>
#include <cstdint>
#include <limits>

#include "application/monitoring/monitoring_schedule.h"

int main()
{
    spark::MonitoringSchedule schedule(1'000);
    auto due = schedule.poll(10'999);
    assert(!due.ping && !due.network);

    due = schedule.poll(11'000);
    assert(due.ping && !due.network);
    due = schedule.poll(61'000);
    assert(due.ping && due.network);

    due = schedule.poll(301'001);
    assert(due.ping && due.network);
    due = schedule.poll(301'001);
    assert(!due.ping && !due.network);
    due = schedule.poll(300'000);
    assert(!due.ping && !due.network);
    due = schedule.poll(311'000);
    assert(due.ping && !due.network);

    spark::MonitoringSchedule negative_origin(-60'000);
    due = negative_origin.poll(0);
    assert(due.ping && due.network);

    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();
    spark::MonitoringSchedule near_max(k_max - spark::MonitoringSchedule::kPingIntervalMs);
    due = near_max.poll(k_max);
    assert(due.ping && !due.network);
    due = near_max.poll(k_max);
    assert(!due.ping && !due.network);

    spark::MonitoringSchedule exhausted(k_max);
    due = exhausted.poll(k_max);
    assert(!due.ping && !due.network);
    return 0;
}
