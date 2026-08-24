#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "core/stats/statistics_service.h"

namespace spark {
namespace {

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double clampUsage(double value)
{
    return (std::max)(0.0, (std::min)(1.0, value));
}

}  // namespace

std::int64_t StatisticsService::effectiveStart(std::int64_t now_ms, std::int64_t window_ms) const
{
    std::int64_t start = (std::max)(start_steady_ms_, now_ms - window_ms);
    if (tick_size_ == ticks_.size()) {
        start = (std::max)(start, ticks_[tick_begin_].steady_ms);
    }
    return (std::min)(start, now_ms);
}

RollingValue StatisticsService::tpsFor(std::int64_t now_ms, std::int64_t window_ms) const
{
    RollingValue result;
    const std::int64_t start = effectiveStart(now_ms, window_ms);
    result.span_ms = now_ms - start;
    if (result.span_ms <= 0) {
        return result;
    }

    for (std::size_t i = 0; i < tick_size_; ++i) {
        const TickSample &sample = ticks_[(tick_begin_ + tick_size_ - 1 - i) % ticks_.size()];
        if (sample.steady_ms <= start) {
            break;
        }
        if (sample.steady_ms <= now_ms) {
            ++result.samples;
        }
    }
    result.present = true;
    result.value = static_cast<double>(result.samples) * 1000.0 / static_cast<double>(result.span_ms);
    return result;
}

DistributionValues StatisticsService::msptFor(std::int64_t now_ms, std::int64_t window_ms) const
{
    DistributionValues result;
    const std::int64_t start = effectiveStart(now_ms, window_ms);
    result.span_ms = now_ms - start;

    std::vector<double> values;
    values.reserve(tick_size_);
    double total = 0.0;
    for (std::size_t i = 0; i < tick_size_; ++i) {
        const TickSample &sample = ticks_[(tick_begin_ + i) % ticks_.size()];
        if (sample.steady_ms > start && sample.steady_ms <= now_ms && sample.duration_valid) {
            values.push_back(sample.duration_ms);
            total += sample.duration_ms;
        }
    }
    if (values.empty()) {
        return result;
    }

    std::ranges::sort(values);
    result.present = true;
    result.samples = values.size();
    result.mean = total / static_cast<double>(values.size());
    result.min = values.front();
    result.max = values.back();
    const std::size_t middle = values.size() / 2;
    result.median = values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
    const std::size_t percentile_index =
        (std::min)(values.size() - 1,
                   static_cast<std::size_t>(std::ceil(static_cast<double>(values.size()) * 0.95)) - 1);
    result.percentile95 = values[percentile_index];
    return result;
}

DistributionValues StatisticsService::msptForRecentSamples(std::size_t max_samples) const
{
    DistributionValues result;
    std::vector<double> values;
    values.reserve((std::min)(max_samples, tick_size_));
    double total = 0.0;
    for (std::size_t i = 0; i < tick_size_ && values.size() < max_samples; ++i) {
        const TickSample &sample = ticks_[(tick_begin_ + tick_size_ - 1 - i) % ticks_.size()];
        if (sample.duration_valid) {
            values.push_back(sample.duration_ms);
            total += sample.duration_ms;
        }
    }
    if (values.empty()) {
        return result;
    }

    std::ranges::sort(values);
    result.present = true;
    result.samples = values.size();
    result.mean = total / static_cast<double>(values.size());
    result.min = values.front();
    result.median = values[static_cast<std::size_t>(std::ceil(0.50 * static_cast<double>(values.size() - 1)))];
    result.percentile95 = values[static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(values.size() - 1)))];
    result.max = values.back();
    return result;
}

RollingValue StatisticsService::cpuFor(std::int64_t now_ms, std::int64_t window_ms, bool process) const
{
    RollingValue result;
    const std::int64_t start = (std::max)(start_steady_ms_, now_ms - window_ms);
    double weighted_total = 0.0;
    std::int64_t covered_ms = 0;

    for (std::size_t i = 0; i < cpu_size_; ++i) {
        const CpuSample &sample = cpu_[(cpu_begin_ + cpu_size_ - 1 - i) % cpu_.size()];
        if (sample.end_steady_ms <= start) {
            break;
        }
        const bool valid = process ? sample.process_valid : sample.system_valid;
        if (!valid) {
            continue;
        }
        const std::int64_t overlap_start = (std::max)(start, sample.start_steady_ms);
        const std::int64_t overlap_end = (std::min)(now_ms, sample.end_steady_ms);
        if (overlap_end <= overlap_start) {
            continue;
        }
        const std::int64_t overlap_ms = overlap_end - overlap_start;
        weighted_total += (process ? sample.process : sample.system) * static_cast<double>(overlap_ms);
        covered_ms += overlap_ms;
        ++result.samples;
    }

    if (covered_ms <= 0) {
        return result;
    }
    result.present = true;
    result.span_ms = covered_ms;
    result.value = clampUsage(weighted_total / static_cast<double>(covered_ms));
    return result;
}

StatisticsSnapshot StatisticsService::snapshot() const
{
    return snapshotAt(steadyNowMs());
}

RollingValue StatisticsService::placeholderTps(std::int64_t window_ms) const
{
    if (!started_ || window_ms <= 0) {
        return {};
    }
    return tpsFor((std::max)(steadyNowMs(), last_observation_steady_ms_), window_ms);
}

DistributionValues StatisticsService::placeholderTickDuration(std::size_t max_samples) const
{
    if (!started_ || max_samples == 0) {
        return {};
    }
    return msptForRecentSamples(max_samples);
}

RollingValue StatisticsService::placeholderCpu(std::int64_t window_ms, bool process) const
{
    if (!started_ || window_ms <= 0) {
        return {};
    }
    return cpuFor((std::max)(steadyNowMs(), last_observation_steady_ms_), window_ms, process);
}

StatisticsSnapshot StatisticsService::snapshotAt(std::int64_t steady_ms) const
{
    StatisticsSnapshot result;
    if (!started_) {
        return result;
    }
    const std::int64_t now_ms = (std::max)(steady_ms, last_observation_steady_ms_);
    result.generated_time_ms = unixTimeFor(now_ms);
    result.history_span_ms = (std::min)(kMaximumHistoryMs, now_ms - start_steady_ms_);

    result.tps.last_5s = tpsFor(now_ms, 5 * 1000);
    result.tps.last_10s = tpsFor(now_ms, 10 * 1000);
    result.tps.last_1m = tpsFor(now_ms, 60 * 1000);
    result.tps.last_5m = tpsFor(now_ms, 5 * 60 * 1000);
    result.tps.last_15m = tpsFor(now_ms, 15 * 60 * 1000);

    result.mspt.last_10s = msptFor(now_ms, 10 * 1000);
    result.mspt.last_1m = msptFor(now_ms, 60 * 1000);
    result.mspt.last_5m = msptFor(now_ms, 5 * 60 * 1000);

    result.cpu.process_last_10s = cpuFor(now_ms, 10 * 1000, true);
    result.cpu.process_last_1m = cpuFor(now_ms, 60 * 1000, true);
    result.cpu.process_last_15m = cpuFor(now_ms, 15 * 60 * 1000, true);
    result.cpu.system_last_10s = cpuFor(now_ms, 10 * 1000, false);
    result.cpu.system_last_1m = cpuFor(now_ms, 60 * 1000, false);
    result.cpu.system_last_15m = cpuFor(now_ms, 15 * 60 * 1000, false);
    return result;
}

}  // namespace spark
