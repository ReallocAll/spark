#include <cassert>
#include <cmath>

#include "core/stats/metrics_history.h"
#include "core/stats/statistics_service.h"

namespace {

spark::CpuSnapshot initialCpu()
{
    spark::CpuSnapshot result;
    result.valid = true;
    result.process_ticks_per_second = 100.0;
    result.cpu_threads = 2;
    return result;
}

void testRingRetentionAndOrder()
{
    spark::MetricsHistory history(3);
    assert(history.recordTps(1'000, 1.0));
    assert(!history.recordTps(11'000, 2.0));
    assert(history.recordTps(11'001, 2.0));
    assert(history.recordTps(21'002, 3.0));
    assert(history.recordTps(32'003, 4.0));

    const auto snapshot = history.snapshot();
    assert(snapshot.tps.size() == 3);
    assert(snapshot.tps[0].timestamp_ms == 11'001);
    assert(snapshot.tps[1].timestamp_ms == 21'002);
    assert(snapshot.tps[2].timestamp_ms == 32'003);
    assert(snapshot.tps[0].value == 2.0);

    spark::MetricsHistory retention;
    assert(!retention.recordTps(0, 1.0));
    assert(retention.recordTps(1, 1.0));
    assert(retention.recordTps(spark::MetricsHistory::kRetentionMs + 2, 2.0));
    assert(retention.snapshot().tps.size() == 1);
    assert(retention.snapshot().tps.front().value == 2.0);
}

void testStatisticsRecordingDelayAndAnchors()
{
    spark::StatisticsService statistics;
    statistics.startAt(1'000, 5'000'000, initialCpu());
    statistics.recordPlayerCountAt(2, 1'000);
    statistics.recordWorldGaugesAt(10, 20, 1'000);
    for (std::int64_t timestamp = 1'500; timestamp < 11'000; timestamp += 500) {
        statistics.recordTickAt(10.0, timestamp);
    }
    assert(statistics.metricsSnapshot().empty());

    statistics.recordTickAt(20.0, 11'001);
    statistics.recordPlayerCountAt(4, 11'001);
    statistics.recordWorldGaugesAt(30, 40, 11'001);
    statistics.recordPlayerPingAt({.mean = 30.0, .max = 50.0, .min = 10.0, .median = 30.0, .percentile95 = 50.0},
                                  11'001);

    const spark::MetricsSnapshot snapshot = statistics.metricsSnapshot();
    assert(snapshot.tps.size() == 1);
    assert(snapshot.tick_duration.size() == 1);
    assert(snapshot.world_info.size() == 1);
    assert(snapshot.player_ping.size() == 1);
    assert(snapshot.tps.front().timestamp_ms == 5'010'001);
    assert(snapshot.world_info.front().players == 4);
    assert(snapshot.world_info.front().entities == 30);
    assert(snapshot.world_info.front().chunks == 40);
    assert(snapshot.world_info.front().tile_entities == 0);
    assert(snapshot.player_ping.front().values.mean == 30.0);
    assert(std::isfinite(snapshot.tps.front().value));
}

}  // namespace

int main()
{
    testRingRetentionAndOrder();
    testStatisticsRecordingDelayAndAnchors();
    return 0;
}
