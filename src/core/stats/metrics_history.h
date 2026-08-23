#ifndef ENDSTONE_SPARK_METRICS_HISTORY_H
#define ENDSTONE_SPARK_METRICS_HISTORY_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spark {

struct MetricsDoubleSample {
    std::int64_t timestamp_ms = 0;
    double value = 0.0;
};

struct MetricsAverages {
    double mean = 0.0;
    double max = 0.0;
    double min = 0.0;
    double median = 0.0;
    double percentile95 = 0.0;
};

struct MetricsAveragesSample {
    std::int64_t timestamp_ms = 0;
    MetricsAverages values;
};

struct MetricsWorldInfoSample {
    std::int64_t timestamp_ms = 0;
    std::int32_t players = 0;
    std::int32_t entities = 0;
    std::int32_t tile_entities = 0;
    std::int32_t chunks = 0;
};

// An immutable-by-convention copy of the metric series at one export point.
struct MetricsSnapshot {
    std::vector<MetricsDoubleSample> tps;
    std::vector<MetricsAveragesSample> tick_duration;
    std::vector<MetricsDoubleSample> cpu_usage_process;
    std::vector<MetricsDoubleSample> cpu_usage_system;
    std::vector<MetricsWorldInfoSample> world_info;
    std::vector<MetricsAveragesSample> player_ping;

    [[nodiscard]] bool empty() const
    {
        return tps.empty() && tick_duration.empty() && cpu_usage_process.empty() && cpu_usage_system.empty() &&
               world_info.empty() && player_ping.empty();
    }
};

// Fixed-retention chronological metric storage matching spark's MetricSeries.
// Samples are copied out in chronological order for safe export-time encoding.
class MetricsHistory {
public:
    static constexpr std::int64_t kRetentionMs = 60 * 60 * 1000;
    static constexpr std::int64_t kIntervalMs = 10 * 1000;
    static constexpr std::size_t kInitialCapacity = static_cast<std::size_t>(kRetentionMs / kIntervalMs) + 1;

    explicit MetricsHistory(std::size_t capacity = kInitialCapacity);

    void clear();

    // Each record method applies the nominal interval independently, as the
    // upstream series do. Older or equal timestamps are rejected.
    bool recordTps(std::int64_t timestamp_ms, double value);
    bool recordTickDuration(std::int64_t timestamp_ms, const MetricsAverages &value);
    bool recordCpuUsageProcess(std::int64_t timestamp_ms, double value);
    bool recordCpuUsageSystem(std::int64_t timestamp_ms, double value);
    bool recordWorldInfo(std::int64_t timestamp_ms, std::int32_t players, std::int32_t entities, std::int32_t chunks,
                         std::int32_t tile_entities = 0);
    bool recordPlayerPing(std::int64_t timestamp_ms, const MetricsAverages &value);

    [[nodiscard]] MetricsSnapshot snapshot() const;

private:
    struct DoubleEntry {
        std::int64_t timestamp_ms = 0;
        double value = 0.0;
    };
    struct AveragesEntry {
        std::int64_t timestamp_ms = 0;
        MetricsAverages values;
    };
    struct WorldEntry {
        std::int64_t timestamp_ms = 0;
        std::int32_t players = 0;
        std::int32_t entities = 0;
        std::int32_t tile_entities = 0;
        std::int32_t chunks = 0;
    };

    bool due(std::int64_t timestamp_ms, std::size_t size, std::int64_t newest_timestamp_ms) const;
    bool appendDouble(std::vector<DoubleEntry> &series, std::size_t &head, std::size_t &size, std::int64_t timestamp_ms,
                      double value);
    bool appendAverages(std::vector<AveragesEntry> &series, std::size_t &head, std::size_t &size,
                        std::int64_t timestamp_ms, const MetricsAverages &value);
    bool appendWorld(std::int64_t timestamp_ms, std::int32_t players, std::int32_t entities, std::int32_t chunks,
                     std::int32_t tile_entities);

    std::vector<DoubleEntry> tps_;
    std::vector<AveragesEntry> tick_duration_;
    std::vector<DoubleEntry> cpu_usage_process_;
    std::vector<DoubleEntry> cpu_usage_system_;
    std::vector<WorldEntry> world_info_;
    std::vector<AveragesEntry> player_ping_;
    std::size_t tps_head_ = 0;
    std::size_t tps_size_ = 0;
    std::size_t tick_duration_head_ = 0;
    std::size_t tick_duration_size_ = 0;
    std::size_t cpu_usage_process_head_ = 0;
    std::size_t cpu_usage_process_size_ = 0;
    std::size_t cpu_usage_system_head_ = 0;
    std::size_t cpu_usage_system_size_ = 0;
    std::size_t world_info_head_ = 0;
    std::size_t world_info_size_ = 0;
    std::size_t player_ping_head_ = 0;
    std::size_t player_ping_size_ = 0;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_METRICS_HISTORY_H
