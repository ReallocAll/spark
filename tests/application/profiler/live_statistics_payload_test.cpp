#include <cassert>

#include "application/profiler/live_statistics_payload.h"
#include "proto/proto_reader.h"

namespace {

bool hasField(std::string_view bytes, int target)
{
    spark::ProtoReader reader(bytes);
    int field = 0;
    int wire_type = 0;
    while (reader.nextField(field, wire_type)) {
        if (field == target) {
            return true;
        }
        reader.skip(wire_type);
    }
    return false;
}

}  // namespace

int main()
{
    spark::ExportContext context;
    context.player_count = 3;
    context.online_mode = 2;
    context.uptime_ms = 12'345;
    context.ping_samples = {20, 40, 60};
    context.statistics.tps.last_1m = {.present = true, .value = 19.5};
    context.statistics.cpu.process_last_1m = {.present = true, .value = 0.25};
    context.metrics.tps.push_back({.timestamp_ms = 12'000, .value = 19.5});
    context.system_stats.cpu_present = true;
    context.system_stats.cpu_threads = 8;

    spark::NetworkInterfaceSnapshot network;
    network.rx_bytes_per_second.present = true;
    network.rx_bytes_per_second.mean = 10.0;
    context.net_snapshots.emplace("eth0", network);

    const spark::LiveStatisticsPayload payload = spark::buildLiveStatisticsPayload(context);
    assert(hasField(payload.platform, 3));
    assert(hasField(payload.platform, 7));
    assert(hasField(payload.platform, 9));
    assert(hasField(payload.platform, 4));
    assert(hasField(payload.platform, 6));
    assert(hasField(payload.system, 1));
    assert(hasField(payload.system, 8));
    assert(hasField(payload.metrics, 1));
    return 0;
}
