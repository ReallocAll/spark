#include "core/stats/metrics_history.h"

#include <algorithm>
#include <limits>

namespace spark {

MetricsHistory::MetricsHistory(std::size_t capacity)
    : tps_(capacity == 0 ? 1 : capacity), tick_duration_(capacity == 0 ? 1 : capacity),
      cpu_usage_process_(capacity == 0 ? 1 : capacity), cpu_usage_system_(capacity == 0 ? 1 : capacity),
      world_info_(capacity == 0 ? 1 : capacity), player_ping_(capacity == 0 ? 1 : capacity)
{
}

void MetricsHistory::clear()
{
    tps_head_ = 0;
    tps_size_ = 0;
    tick_duration_head_ = 0;
    tick_duration_size_ = 0;
    cpu_usage_process_head_ = 0;
    cpu_usage_process_size_ = 0;
    cpu_usage_system_head_ = 0;
    cpu_usage_system_size_ = 0;
    world_info_head_ = 0;
    world_info_size_ = 0;
    player_ping_head_ = 0;
    player_ping_size_ = 0;
}

bool MetricsHistory::due(std::int64_t timestamp_ms, std::size_t size, std::int64_t newest_timestamp_ms)
{
    if (timestamp_ms <= 0 || size == 0) {
        return timestamp_ms > 0;
    }
    return newest_timestamp_ms < timestamp_ms - kIntervalMs;
}

bool MetricsHistory::appendDouble(std::vector<DoubleEntry> &series, std::size_t &head, std::size_t &size,
                                  std::int64_t timestamp_ms, double value)
{
    if (!due(timestamp_ms, size, size == 0 ? 0 : series[(head + size - 1) % series.size()].timestamp_ms)) {
        return false;
    }
    const std::int64_t cutoff = timestamp_ms > kRetentionMs ? timestamp_ms - kRetentionMs : 0;
    while (size > 0 && series[head].timestamp_ms < cutoff) {
        head = (head + 1) % series.size();
        --size;
    }
    if (size == series.size()) {
        series[head] = {.timestamp_ms = timestamp_ms, .value = value};
        head = (head + 1) % series.size();
    }
    else {
        const std::size_t index = (head + size) % series.size();
        series[index] = {.timestamp_ms = timestamp_ms, .value = value};
        ++size;
    }
    return true;
}

bool MetricsHistory::appendAverages(std::vector<AveragesEntry> &series, std::size_t &head, std::size_t &size,
                                    std::int64_t timestamp_ms, const MetricsAverages &value)
{
    if (!due(timestamp_ms, size, size == 0 ? 0 : series[(head + size - 1) % series.size()].timestamp_ms)) {
        return false;
    }
    const std::int64_t cutoff = timestamp_ms > kRetentionMs ? timestamp_ms - kRetentionMs : 0;
    while (size > 0 && series[head].timestamp_ms < cutoff) {
        head = (head + 1) % series.size();
        --size;
    }
    if (size == series.size()) {
        series[head] = {.timestamp_ms = timestamp_ms, .values = value};
        head = (head + 1) % series.size();
    }
    else {
        const std::size_t index = (head + size) % series.size();
        series[index] = {.timestamp_ms = timestamp_ms, .values = value};
        ++size;
    }
    return true;
}

bool MetricsHistory::appendWorld(std::int64_t timestamp_ms, std::int32_t players, std::int32_t entities,
                                 std::int32_t chunks, std::int32_t tile_entities)
{
    if (!due(timestamp_ms, world_info_size_,
             world_info_size_ == 0
                 ? 0
                 : world_info_[(world_info_head_ + world_info_size_ - 1) % world_info_.size()].timestamp_ms)) {
        return false;
    }
    const std::int64_t cutoff = timestamp_ms > kRetentionMs ? timestamp_ms - kRetentionMs : 0;
    while (world_info_size_ > 0 && world_info_[world_info_head_].timestamp_ms < cutoff) {
        world_info_head_ = (world_info_head_ + 1) % world_info_.size();
        --world_info_size_;
    }
    const WorldEntry value = {.timestamp_ms = timestamp_ms,
                              .players = players,
                              .entities = entities,
                              .tile_entities = tile_entities,
                              .chunks = chunks};
    if (world_info_size_ == world_info_.size()) {
        world_info_[world_info_head_] = value;
        world_info_head_ = (world_info_head_ + 1) % world_info_.size();
    }
    else {
        const std::size_t index = (world_info_head_ + world_info_size_) % world_info_.size();
        world_info_[index] = value;
        ++world_info_size_;
    }
    return true;
}

bool MetricsHistory::recordTps(std::int64_t timestamp_ms, double value)
{
    return appendDouble(tps_, tps_head_, tps_size_, timestamp_ms, value);
}

bool MetricsHistory::recordTickDuration(std::int64_t timestamp_ms, const MetricsAverages &value)
{
    return appendAverages(tick_duration_, tick_duration_head_, tick_duration_size_, timestamp_ms, value);
}

bool MetricsHistory::recordCpuUsageProcess(std::int64_t timestamp_ms, double value)
{
    return appendDouble(cpu_usage_process_, cpu_usage_process_head_, cpu_usage_process_size_, timestamp_ms, value);
}

bool MetricsHistory::recordCpuUsageSystem(std::int64_t timestamp_ms, double value)
{
    return appendDouble(cpu_usage_system_, cpu_usage_system_head_, cpu_usage_system_size_, timestamp_ms, value);
}

bool MetricsHistory::recordWorldInfo(std::int64_t timestamp_ms, std::int32_t players, std::int32_t entities,
                                     std::int32_t chunks, std::int32_t tile_entities)
{
    return appendWorld(timestamp_ms, players, entities, chunks, tile_entities);
}

bool MetricsHistory::recordPlayerPing(std::int64_t timestamp_ms, const MetricsAverages &value)
{
    return appendAverages(player_ping_, player_ping_head_, player_ping_size_, timestamp_ms, value);
}

MetricsSnapshot MetricsHistory::snapshot() const
{
    MetricsSnapshot result;
    auto copy_double = [](const std::vector<DoubleEntry> &series, std::size_t head, std::size_t size,
                          std::vector<MetricsDoubleSample> &out) {
        out.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            const DoubleEntry &entry = series[(head + i) % series.size()];
            out.push_back({.timestamp_ms = entry.timestamp_ms, .value = entry.value});
        }
    };
    auto copy_averages = [](const std::vector<AveragesEntry> &series, std::size_t head, std::size_t size,
                            std::vector<MetricsAveragesSample> &out) {
        out.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            const AveragesEntry &entry = series[(head + i) % series.size()];
            out.push_back({.timestamp_ms = entry.timestamp_ms, .values = entry.values});
        }
    };
    copy_double(tps_, tps_head_, tps_size_, result.tps);
    copy_averages(tick_duration_, tick_duration_head_, tick_duration_size_, result.tick_duration);
    copy_double(cpu_usage_process_, cpu_usage_process_head_, cpu_usage_process_size_, result.cpu_usage_process);
    copy_double(cpu_usage_system_, cpu_usage_system_head_, cpu_usage_system_size_, result.cpu_usage_system);
    copy_averages(player_ping_, player_ping_head_, player_ping_size_, result.player_ping);
    result.world_info.reserve(world_info_size_);
    for (std::size_t i = 0; i < world_info_size_; ++i) {
        const WorldEntry &entry = world_info_[(world_info_head_ + i) % world_info_.size()];
        result.world_info.push_back({.timestamp_ms = entry.timestamp_ms,
                                     .players = entry.players,
                                     .entities = entry.entities,
                                     .tile_entities = entry.tile_entities,
                                     .chunks = entry.chunks});
    }
    return result;
}

}  // namespace spark
