#include <cassert>
#include <cmath>
#include <limits>

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

void testMemoryRingRetentionAndPresence()
{
    spark::MetricsHistory history(2);
    assert(history.recordMemoryUsage(
        1'000, {.used = 100, .committed_present = true, .committed = 200, .max_present = true, .max = 300}));
    assert(!history.recordMemoryUsage(11'000, {.used = 101}));
    assert(history.recordMemoryUsage(11'001, {.used = 101}));
    assert(history.recordMemoryUsage(21'002, {.used = 102, .max_present = true, .max = 400}));

    const spark::MetricsSnapshot snapshot = history.snapshot();
    assert(snapshot.memory_usage_heap.size() == 2);
    assert(snapshot.memory_usage_heap[0].timestamp_ms == 11'001);
    assert(snapshot.memory_usage_heap[0].values.used == 101);
    assert(!snapshot.memory_usage_heap[0].values.committed_present);
    assert(snapshot.memory_usage_heap[1].values.used == 102);
    assert(snapshot.memory_usage_heap[1].values.max_present);
    assert(snapshot.memory_usage_heap[1].values.max == 400);
}

void testStatisticsRecordingDelayAndAnchors()
{
    spark::StatisticsService statistics;
    statistics.startAt(1'000, 5'000'000, initialCpu());
    statistics.recordPlayerCountAt(2, 1'000);
    statistics.recordWorldGaugesAt(10, 0, 20, false, 1'000);
    for (std::int64_t timestamp = 1'500; timestamp < 11'000; timestamp += 500) {
        statistics.recordTickAt(10.0, timestamp);
    }
    assert(statistics.metricsSnapshot().empty());

    statistics.recordTickAt(20.0, 11'000);
    statistics.recordPlayerCountAt(4, 11'001);
    statistics.recordWorldGaugesAt(30, 7, 40, true, 11'001);
    statistics.recordPlayerPingAt({.mean = 30.0, .max = 50.0, .min = 10.0, .median = 30.0, .percentile95 = 50.0},
                                  11'001);

    statistics.recordTickAt(30.0, 21'000);
    assert(statistics.metricsSnapshot().tps.size() == 1);
    statistics.recordTickAt(40.0, 21'001);

    const spark::MetricsSnapshot snapshot = statistics.metricsSnapshot();
    assert(snapshot.tps.size() == 2);
    assert(snapshot.tick_duration.size() == 2);
    assert(snapshot.memory_usage_heap.size() == 2);
    assert(snapshot.memory_usage_heap.front().values.used > 0);
#ifdef _WIN32
    assert(snapshot.memory_usage_heap.front().values.committed_present);
    assert(snapshot.memory_usage_heap.front().values.committed > 0);
#else
    assert(!snapshot.memory_usage_heap.front().values.committed_present);
#endif
    assert(snapshot.world_info.size() == 1);
    assert(snapshot.player_ping.size() == 1);
    assert(snapshot.tps.front().timestamp_ms == 5'010'000);
    assert(snapshot.tps.back().timestamp_ms == 5'020'001);
    assert(snapshot.memory_usage_heap.front().timestamp_ms == 5'010'000);
    assert(snapshot.memory_usage_heap.back().timestamp_ms == 5'020'001);
    assert(snapshot.world_info.front().players == 4);
    assert(snapshot.world_info.front().entities == 30);
    assert(snapshot.world_info.front().chunks == 40);
    assert(snapshot.world_info.front().tile_entities == 7);
    assert(snapshot.world_info.front().tile_entities_present);
    assert(snapshot.player_ping.front().values.mean == 30.0);
    assert(std::isfinite(snapshot.tps.front().value));
}

void testAllocationRateRollingAndMetrics()
{
    spark::StatisticsService statistics;
    statistics.startAt(1'000, 5'000'000, initialCpu());
    statistics.recordAllocationBytesAt(0, 1'000);

    // A rate sample must use the same nominal 10-second cadence as MetricsHistory.
    statistics.recordAllocationBytesAt(9'999, 10'999);
    assert(!statistics.snapshotAt(10'999).allocation.last_1m.present);
    assert(statistics.metricsSnapshot().memory_allocation.empty());

    // Use a deliberately non-round elapsed time to prove the rate uses the
    // real timestamp delta instead of assuming exactly 10.000 seconds.
    statistics.recordAllocationBytesAt(10'250, 11'250);
    spark::StatisticsSnapshot rolling = statistics.snapshotAt(11'250);
    assert(rolling.allocation.last_1m.present);
    assert(rolling.allocation.last_5m.present);
    assert(rolling.allocation.last_15m.present);
    assert(std::abs(rolling.allocation.last_1m.mean - 1000.0) < 0.001);
    assert(rolling.allocation.last_1m.samples == 1);

    spark::MetricsSnapshot metrics = statistics.metricsSnapshot();
    assert(metrics.memory_allocation.size() == 1);
    assert(std::abs(metrics.memory_allocation.front().value - 1000.0) < 0.001);
    assert(metrics.memory_allocation.front().timestamp_ms == 5'010'250);

    // Less than another 10 seconds must not create a rolling or metric sample.
    statistics.recordAllocationBytesAt(20'000, 21'000);
    assert(statistics.snapshotAt(21'000).allocation.last_1m.samples == 1);
    assert(statistics.metricsSnapshot().memory_allocation.size() == 1);

    statistics.recordAllocationBytesAt(21'000, 22'000);
    rolling = statistics.snapshotAt(22'000);
    assert(rolling.allocation.last_1m.samples == 2);
    assert(std::abs(rolling.allocation.last_1m.mean - 1000.0) < 0.001);
    metrics = statistics.metricsSnapshot();
    assert(metrics.memory_allocation.size() == 2);
    assert(metrics.memory_allocation.back().timestamp_ms == 5'021'000);
    assert(std::abs(metrics.memory_allocation.back().value - 1000.0) < 0.001);

    // A defensive counter reset/wrap establishes a fresh baseline instead of
    // emitting a huge wrapped rate.
    statistics.recordAllocationBytesAt(1, 23'000);
    assert(statistics.snapshotAt(23'000).allocation.last_1m.samples == 2);
    statistics.recordAllocationBytesAt(10'501, 33'500);
    rolling = statistics.snapshotAt(33'500);
    assert(rolling.allocation.last_1m.samples == 3);
    assert(std::abs(rolling.allocation.last_1m.mean - 1000.0) < 0.001);
}

void testStatisticsCadenceOverflow()
{
    spark::StatisticsService statistics;
    constexpr std::int64_t start = std::numeric_limits<std::int64_t>::max() - 5'000;
    statistics.startAt(start, 5'000'000, initialCpu());
    statistics.recordTickAt(10.0, std::numeric_limits<std::int64_t>::max());
    assert(statistics.metricsSnapshot().empty());
}

}  // namespace

int main()
{
    testRingRetentionAndOrder();
    testMemoryRingRetentionAndPresence();
    testStatisticsRecordingDelayAndAnchors();
    testAllocationRateRollingAndMetrics();
    testStatisticsCadenceOverflow();
    return 0;
}
