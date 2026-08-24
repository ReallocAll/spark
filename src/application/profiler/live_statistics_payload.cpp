#include "application/profiler/live_statistics_payload.h"

#include "core/stats/ping_statistics.h"
#include "core/stats/system_stats.h"
#include "proto/metrics_proto.h"
#include "proto/statistics_proto.h"

namespace spark {

LiveStatisticsPayload buildLiveStatisticsPayload(const ExportContext &context)
{
    PlatformStats platform;
    platform.present = true;
    platform.player_count = context.player_count;
    platform.online_mode = context.online_mode;
    platform.uptime_ms = context.uptime_ms;

    const ProcessStats process = gatherProcessStats();
    platform.process_mem_present = process.rss_present;
    platform.process_mem_bytes = process.rss_bytes;
    platform.process_virtual_present = process.virtual_present;
    platform.process_virtual_bytes = process.virtual_bytes;

    if (!context.ping_samples.empty()) {
        PingRollingAverage average(PingStatistics::kWindowSize);
        for (const int value : context.ping_samples) {
            average.add(value);
        }
        platform.ping_present = true;
        platform.ping_mean = average.mean();
        platform.ping_max = static_cast<double>(average.max());
        platform.ping_min = static_cast<double>(average.min());
        platform.ping_median = static_cast<double>(average.median());
        platform.ping_p95 = static_cast<double>(average.percentile95th());
    }

    SystemStats system = context.system_stats;
    system.present = true;
    system.uptime_present = true;
    system.uptime_ms = context.uptime_ms;
    system.net_present = !context.net_snapshots.empty();
    system.net_averages = context.net_snapshots;

    return {.platform = proto_detail::buildPlatformStatistics(platform, context.statistics),
            .system = proto_detail::buildSystemStatistics(system, context.statistics),
            .metrics = proto_detail::buildMetrics(context.metrics)};
}

}  // namespace spark
