#include <algorithm>
#include <map>
#include <vector>

#include "core/stats/statistics_service.h"

namespace spark {

std::map<std::int32_t, WindowStats> StatisticsService::profileWindows(std::int64_t profile_start_unix_ms,
                                                                      std::int64_t profile_end_unix_ms) const
{
    std::map<std::int32_t, WindowStats> result;
    if (!started_ || profile_end_unix_ms <= profile_start_unix_ms) {
        return result;
    }

    const std::int32_t adjustment_ms = profiling_window::windowAdjustmentMs();
    const std::int64_t available_start =
        (std::max)({profile_start_unix_ms, start_unix_ms_, profile_end_unix_ms - kMaximumHistoryMs});
    if (available_start >= profile_end_unix_ms) {
        return result;
    }

    const std::int32_t first_window = profiling_window::timeToWindow(available_start, adjustment_ms);
    const std::int32_t last_window = profiling_window::timeToWindow(profile_end_unix_ms - 1, adjustment_ms);
    if (first_window > last_window) {
        return result;
    }

    struct Accumulator {
        WindowStats stats;
        std::vector<double> durations;
        double process_weighted = 0.0;
        double system_weighted = 0.0;
        std::int64_t process_covered_ms = 0;
        std::int64_t system_covered_ms = 0;
    };
    std::map<std::int32_t, Accumulator> accumulators;

    for (std::int64_t window = first_window; window <= last_window; ++window) {
        const auto window_id = static_cast<std::int32_t>(window);
        Accumulator &accumulator = accumulators[window_id];
        const std::int64_t nominal_start = profiling_window::windowStartTime(window_id, adjustment_ms);
        const std::int64_t nominal_end = profiling_window::windowEndTime(window_id, adjustment_ms);
        const std::int64_t start = (std::max)(available_start, nominal_start);
        const std::int64_t end = (std::min)(profile_end_unix_ms, nominal_end);
        accumulator.stats.ticks_present = true;
        accumulator.stats.tps_present = true;
        accumulator.stats.start_time_ms = start;
        accumulator.stats.end_time_ms = end;
        accumulator.stats.duration_ms = static_cast<int>((std::max<std::int64_t>)(0, end - start));
    }

    for (std::size_t i = 0; i < tick_size_; ++i) {
        const TickSample &sample = ticks_[(tick_begin_ + i) % ticks_.size()];
        const std::int64_t sample_unix_ms = unixTimeFor(sample.steady_ms);
        if (sample_unix_ms < available_start || sample_unix_ms >= profile_end_unix_ms) {
            continue;
        }
        const std::int32_t window = profiling_window::timeToWindow(sample_unix_ms, adjustment_ms);
        auto accumulator_it = accumulators.find(window);
        if (accumulator_it == accumulators.end()) {
            continue;
        }
        Accumulator &accumulator = accumulator_it->second;
        ++accumulator.stats.ticks;
        if (sample.duration_valid) {
            accumulator.durations.push_back(sample.duration_ms);
        }
    }

    for (std::size_t i = 0; i < cpu_size_; ++i) {
        const CpuSample &sample = cpu_[(cpu_begin_ + i) % cpu_.size()];
        for (std::int64_t window = first_window; window <= last_window; ++window) {
            Accumulator &accumulator = accumulators.at(static_cast<std::int32_t>(window));
            const std::int64_t window_start = start_steady_ms_ + (accumulator.stats.start_time_ms - start_unix_ms_);
            const std::int64_t window_end = start_steady_ms_ + (accumulator.stats.end_time_ms - start_unix_ms_);
            const std::int64_t overlap_start = (std::max)(window_start, sample.start_steady_ms);
            const std::int64_t overlap_end = (std::min)(window_end, sample.end_steady_ms);
            if (overlap_end <= overlap_start) {
                continue;
            }
            const std::int64_t overlap_ms = overlap_end - overlap_start;
            if (sample.process_valid) {
                accumulator.process_weighted += sample.process * static_cast<double>(overlap_ms);
                accumulator.process_covered_ms += overlap_ms;
            }
            if (sample.system_valid) {
                accumulator.system_weighted += sample.system * static_cast<double>(overlap_ms);
                accumulator.system_covered_ms += overlap_ms;
            }
        }
    }

    for (std::int64_t window = first_window; window <= last_window; ++window) {
        const auto window_id = static_cast<std::int32_t>(window);
        Accumulator &accumulator = accumulators.at(window_id);
        WindowStats &stats = accumulator.stats;
        if (stats.duration_ms > 0) {
            stats.tps = static_cast<double>(stats.ticks) * 1000.0 / static_cast<double>(stats.duration_ms);
        }

        if (!accumulator.durations.empty()) {
            std::ranges::sort(accumulator.durations);
            stats.mspt_present = true;
            stats.mspt_max = accumulator.durations.back();
            const std::size_t middle = accumulator.durations.size() / 2;
            stats.mspt_median = accumulator.durations.size() % 2 == 0
                                  ? (accumulator.durations[middle - 1] + accumulator.durations[middle]) / 2.0
                                  : accumulator.durations[middle];
        }
        if (accumulator.process_covered_ms > 0) {
            stats.cpu_process_present = true;
            stats.cpu_process = accumulator.process_weighted / static_cast<double>(accumulator.process_covered_ms);
        }
        if (accumulator.system_covered_ms > 0) {
            stats.cpu_system_present = true;
            stats.cpu_system = accumulator.system_weighted / static_cast<double>(accumulator.system_covered_ms);
        }

        for (std::size_t i = 0; i < gauge_size_; ++i) {
            const GaugeSample &gauge = gauges_[(gauge_begin_ + i) % gauges_.size()];
            if (unixTimeFor(gauge.steady_ms) <= stats.end_time_ms) {
                stats.players_present = true;
                stats.players = gauge.players;
                if (gauge.world_gauges_set) {
                    stats.entities_present = true;
                    stats.entities = gauge.entities;
                    stats.chunks_present = true;
                    stats.chunks = gauge.chunks;
                    stats.tile_entities_present = gauge.tile_entities_present;
                    stats.tile_entities = gauge.tile_entities;
                }
            }
        }
        result.emplace(window_id, stats);
    }
    return result;
}

std::int64_t StatisticsService::unixTimeFor(std::int64_t steady_ms) const
{
    return start_unix_ms_ + (steady_ms - start_steady_ms_);
}

}  // namespace spark
