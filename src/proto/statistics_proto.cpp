#include "proto/statistics_proto.h"

#include "proto/proto_writer.h"

namespace spark::proto_detail {

namespace {

void writeDistribution(ProtoWriter &writer, int field, const DistributionValues &distribution)
{
    if (!distribution.present) {
        return;
    }
    std::string values;
    ProtoWriter values_writer(values);
    values_writer.real(1, distribution.mean);
    values_writer.real(2, distribution.max);
    values_writer.real(3, distribution.min);
    values_writer.real(4, distribution.median);
    values_writer.real(5, distribution.percentile95);
    writer.message(field, values);
}

void writeRate(ProtoWriter &writer, int field, const NetworkRateValues &rate)
{
    if (!rate.present) {
        return;
    }
    std::string values;
    ProtoWriter values_writer(values);
    values_writer.real(1, rate.mean);
    values_writer.real(2, rate.max);
    values_writer.real(3, rate.min);
    values_writer.real(4, rate.median);
    values_writer.real(5, rate.percentile95);
    writer.message(field, values);
}

}  // namespace

std::string buildWorldStatistics(const WorldInfo &world)
{
    std::string out;
    ProtoWriter writer(out);
    writer.int32(1, world.total_entities);
    for (const auto &[type, count] : world.entity_counts) {
        std::string entry;
        ProtoWriter entry_writer(entry);
        entry_writer.string(1, type);
        entry_writer.int32(2, count);
        writer.message(2, entry);
    }
    for (const WorldEntry &world_entry : world.worlds) {
        std::string world_bytes;
        ProtoWriter world_writer(world_bytes);
        world_writer.string(1, world_entry.name);
        world_writer.int32(2, world_entry.total_entities);
        for (const WorldRegion &region_entry : world_entry.regions) {
            std::string region;
            ProtoWriter region_writer(region);
            region_writer.int32(1, region_entry.total_entities);
            for (const WorldChunk &chunk_entry : region_entry.chunks) {
                std::string chunk;
                ProtoWriter chunk_writer(chunk);
                chunk_writer.int32(1, chunk_entry.x);
                chunk_writer.int32(2, chunk_entry.z);
                chunk_writer.int32(3, chunk_entry.total_entities);
                for (const auto &[type, count] : chunk_entry.entity_counts) {
                    std::string entity_count;
                    ProtoWriter entity_count_writer(entity_count);
                    entity_count_writer.string(1, type);
                    entity_count_writer.int32(2, count);
                    chunk_writer.message(4, entity_count);
                }
                region_writer.message(2, chunk);
            }
            world_writer.message(3, region);
        }
        writer.message(3, world_bytes);
    }
    for (const GameRuleInfo &game_rule : world.game_rules) {
        std::string rule;
        ProtoWriter rule_writer(rule);
        rule_writer.string(1, game_rule.name);
        if (game_rule.default_value.has_value()) {
            rule_writer.string(2, *game_rule.default_value);
        }
        for (const auto &[world_name, value] : game_rule.world_values) {
            std::string world_value;
            ProtoWriter world_value_writer(world_value);
            world_value_writer.string(1, world_name);
            world_value_writer.string(2, value);
            rule_writer.message(3, world_value);
        }
        writer.message(4, rule);
    }
    return out;
}

std::string buildPlatformStatistics(const PlatformStats &platform, const StatisticsSnapshot &statistics,
                                    const WorldInfo *world)
{
    std::string out;
    ProtoWriter writer(out);

    // Point-in-time native process memory only carries the reliably captured
    // resident value. Virtual address-space reservation (VmSize / reserved VA)
    // is deliberately not serialized as MemoryUsage.committed. Reliable private
    // commit and native limits are carried by Metrics.memory_usage_heap.
    if (platform.process_mem_present) {
        std::string heap;
        ProtoWriter heap_writer(heap);
        heap_writer.int64(1, platform.process_mem_bytes);
        std::string memory;
        ProtoWriter memory_writer(memory);
        memory_writer.message(1, heap);
        writer.message(1, memory);
    }
    writer.int64(3, platform.uptime_ms);
    if (statistics.tps.last_1m.present || statistics.tps.last_5m.present || statistics.tps.last_15m.present) {
        std::string tps;
        ProtoWriter tps_writer(tps);
        if (statistics.tps.last_1m.present) {
            tps_writer.real(1, statistics.tps.last_1m.value);
        }
        if (statistics.tps.last_5m.present) {
            tps_writer.real(2, statistics.tps.last_5m.value);
        }
        if (statistics.tps.last_15m.present) {
            tps_writer.real(3, statistics.tps.last_15m.value);
        }
        tps_writer.int32(4, platform.target_tps);
        writer.message(4, tps);
    }
    if (statistics.mspt.last_1m.present || statistics.mspt.last_5m.present) {
        std::string mspt;
        ProtoWriter mspt_writer(mspt);
        writeDistribution(mspt_writer, 1, statistics.mspt.last_1m);
        writeDistribution(mspt_writer, 2, statistics.mspt.last_5m);
        mspt_writer.int32(3, platform.max_ideal_mspt);
        writer.message(5, mspt);
    }
    if (platform.ping_present) {
        std::string ping;
        ProtoWriter ping_writer(ping);
        std::string values;
        ProtoWriter values_writer(values);
        values_writer.real(1, platform.ping_mean);
        values_writer.real(2, platform.ping_max);
        values_writer.real(3, platform.ping_min);
        values_writer.real(4, platform.ping_median);
        values_writer.real(5, platform.ping_p95);
        ping_writer.message(1, values);
        writer.message(6, ping);
    }
    if (platform.player_count >= 0) {
        writer.int64(7, platform.player_count);
    }
    if (platform.online_mode > 0) {
        writer.varint(9, static_cast<std::uint64_t>(platform.online_mode));
    }
    if (world != nullptr && world->present) {
        writer.message(8, buildWorldStatistics(*world));
    }
    return out;
}

std::string buildSystemStatistics(const SystemStats &system, const StatisticsSnapshot &statistics)
{
    std::string out;
    ProtoWriter writer(out);
    if (system.cpu_present || statistics.cpu.process_last_1m.present || statistics.cpu.process_last_15m.present ||
        statistics.cpu.system_last_1m.present || statistics.cpu.system_last_15m.present) {
        std::string cpu;
        ProtoWriter cpu_writer(cpu);
        if (system.cpu_present && system.cpu_threads > 0) {
            cpu_writer.int32(1, system.cpu_threads);
        }
        if (statistics.cpu.process_last_1m.present || statistics.cpu.process_last_15m.present) {
            std::string usage;
            ProtoWriter usage_writer(usage);
            if (statistics.cpu.process_last_1m.present) {
                usage_writer.real(1, statistics.cpu.process_last_1m.value);
            }
            if (statistics.cpu.process_last_15m.present) {
                usage_writer.real(2, statistics.cpu.process_last_15m.value);
            }
            cpu_writer.message(2, usage);
        }
        if (statistics.cpu.system_last_1m.present || statistics.cpu.system_last_15m.present) {
            std::string usage;
            ProtoWriter usage_writer(usage);
            if (statistics.cpu.system_last_1m.present) {
                usage_writer.real(1, statistics.cpu.system_last_1m.value);
            }
            if (statistics.cpu.system_last_15m.present) {
                usage_writer.real(2, statistics.cpu.system_last_15m.value);
            }
            cpu_writer.message(3, usage);
        }
        if (!system.cpu_model.empty()) {
            cpu_writer.string(4, system.cpu_model);
        }
        writer.message(1, cpu);
    }
    if (system.memory_present || system.swap_present) {
        std::string memory;
        ProtoWriter memory_writer(memory);
        if (system.memory_present) {
            std::string physical;
            ProtoWriter physical_writer(physical);
            physical_writer.int64(1, system.mem_used);
            physical_writer.int64(2, system.mem_total);
            memory_writer.message(1, physical);
        }
        if (system.swap_present) {
            std::string swap;
            ProtoWriter swap_writer(swap);
            swap_writer.int64(1, system.swap_used);
            swap_writer.int64(2, system.swap_total);
            memory_writer.message(2, swap);
        }
        writer.message(2, memory);
    }
    if (system.disk_present) {
        std::string disk;
        ProtoWriter disk_writer(disk);
        disk_writer.int64(1, system.disk_used);
        disk_writer.int64(2, system.disk_total);
        writer.message(4, disk);
    }
    if (system.os_present) {
        std::string os;
        ProtoWriter os_writer(os);
        if (!system.os_arch.empty()) {
            os_writer.string(1, system.os_arch);
        }
        if (!system.os_name.empty()) {
            os_writer.string(2, system.os_name);
        }
        if (!system.os_version.empty()) {
            os_writer.string(3, system.os_version);
        }
        writer.message(5, os);
    }
    // The viewer expects this Platform view entry even though BDS is native.
    writer.message(6, std::string());
    if (system.uptime_present) {
        writer.int64(7, system.uptime_ms);
    }
    if (system.net_present) {
        for (const auto &[name, snapshot] : system.net_averages) {
            std::string network_interface;
            ProtoWriter network_writer(network_interface);
            writeRate(network_writer, 1, snapshot.rx_bytes_per_second);
            writeRate(network_writer, 2, snapshot.tx_bytes_per_second);
            writeRate(network_writer, 3, snapshot.rx_packets_per_second);
            writeRate(network_writer, 4, snapshot.tx_packets_per_second);
            std::string entry;
            ProtoWriter entry_writer(entry);
            entry_writer.string(1, name);
            entry_writer.message(2, network_interface);
            writer.message(8, entry);
        }
    }
    return out;
}

std::string buildWindowStatistics(const WindowStats &window)
{
    std::string out;
    ProtoWriter writer(out);
    if (window.ticks_present) {
        writer.int32(1, window.ticks);
    }
    if (window.cpu_process_present) {
        writer.real(2, window.cpu_process);
    }
    if (window.cpu_system_present) {
        writer.real(3, window.cpu_system);
    }
    if (window.tps_present) {
        writer.real(4, window.tps);
    }
    if (window.mspt_present) {
        writer.real(5, window.mspt_median);
        writer.real(6, window.mspt_max);
    }
    if (window.players_present) {
        writer.int32(7, window.players);
    }
    if (window.entities_present) {
        writer.int32(8, window.entities);
    }
    if (window.tile_entities_present) {
        writer.int32(9, window.tile_entities);
    }
    if (window.chunks_present) {
        writer.int32(10, window.chunks);
    }
    if (window.duration_ms > 0) {
        writer.int64(11, window.start_time_ms);
        writer.int64(12, window.end_time_ms);
        writer.int32(13, window.duration_ms);
    }
    return out;
}

}  // namespace spark::proto_detail
