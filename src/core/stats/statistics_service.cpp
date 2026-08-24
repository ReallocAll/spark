#include "core/stats/statistics_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "core/util/monotonic_time.h"

namespace spark {
namespace {

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t unixNowMs()
{
    return monotonicUnixMillis();
}

double clampUsage(double value)
{
    return (std::max)(0.0, (std::min)(1.0, value));
}

}  // namespace

StatisticsService::StatisticsService() : ticks_(kTickCapacity), cpu_(kCpuCapacity), gauges_(kGaugeCapacity) {}

void StatisticsService::start()
{
    const std::int64_t steady_ms = steadyNowMs();
    CpuSnapshot initial = captureCpuSnapshot();
    startAt(steady_ms, unixNowMs(), initial);
}

void StatisticsService::startAt(std::int64_t steady_ms, std::int64_t unix_ms, const CpuSnapshot &initial_cpu)
{
    tick_begin_ = 0;
    tick_size_ = 0;
    cpu_begin_ = 0;
    cpu_size_ = 0;
    gauge_begin_ = 0;
    gauge_size_ = 0;
    start_steady_ms_ = steady_ms;
    start_unix_ms_ = unix_ms;
    last_observation_steady_ms_ = steady_ms;
    next_cpu_sample_steady_ms_ = steady_ms + 1000;
    previous_cpu_ = initial_cpu;
    metrics_history_.clear();
    started_ = true;
}

bool StatisticsService::onTick(double duration_ms)
{
    if (!started_) {
        start();
    }

    const std::int64_t now_ms = steadyNowMs();
    recordTickAt(duration_ms, now_ms);
    if (now_ms < next_cpu_sample_steady_ms_) {
        return false;
    }

    recordCpuSnapshot(captureCpuSnapshot());
    next_cpu_sample_steady_ms_ = now_ms + 1000;
    return true;
}

void StatisticsService::recordTickAt(double duration_ms, std::int64_t steady_ms)
{
    if (!started_) {
        startAt(steady_ms, steady_ms, CpuSnapshot{});
    }
    steady_ms = std::max(steady_ms, last_observation_steady_ms_);

    TickSample sample;
    sample.steady_ms = steady_ms;
    sample.duration_valid = std::isfinite(duration_ms) && duration_ms >= 0.0;
    sample.duration_ms = sample.duration_valid ? duration_ms : 0.0;

    std::size_t index = (tick_begin_ + tick_size_) % ticks_.size();
    if (tick_size_ == ticks_.size()) {
        index = tick_begin_;
        tick_begin_ = (tick_begin_ + 1) % ticks_.size();
    }
    else {
        ++tick_size_;
    }
    ticks_[index] = sample;
    last_observation_steady_ms_ = steady_ms;
    recordMetricsAt(steady_ms);
}

void StatisticsService::recordCpuSnapshot(const CpuSnapshot &current)
{
    if (!started_ || !previous_cpu_.valid || !current.valid || current.wall_ms <= previous_cpu_.wall_ms) {
        previous_cpu_ = current;
        return;
    }

    const CpuUsage usage = cpuUsageBetween(previous_cpu_, current);
    CpuSample sample;
    sample.start_steady_ms = previous_cpu_.wall_ms;
    sample.end_steady_ms = current.wall_ms;
    sample.process = clampUsage(usage.process);
    sample.system = clampUsage(usage.system);
    sample.process_valid = usage.process_valid;
    sample.system_valid = usage.system_valid;

    std::size_t index = (cpu_begin_ + cpu_size_) % cpu_.size();
    if (cpu_size_ == cpu_.size()) {
        index = cpu_begin_;
        cpu_begin_ = (cpu_begin_ + 1) % cpu_.size();
    }
    else {
        ++cpu_size_;
    }
    cpu_[index] = sample;
    previous_cpu_ = current;
    last_observation_steady_ms_ = (std::max)(last_observation_steady_ms_, current.wall_ms);
    recordMetricsAt(current.wall_ms);
}

void StatisticsService::recordPlayerCount(std::int64_t players)
{
    recordPlayerCountAt(players, last_observation_steady_ms_);
}

void StatisticsService::recordPlayerCountAt(std::int64_t players, std::int64_t steady_ms)
{
    if (!started_ || players < 0) {
        return;
    }

    steady_ms = (std::max)(steady_ms, last_observation_steady_ms_);

    GaugeSample sample;
    sample.steady_ms = steady_ms;
    sample.players = static_cast<int>((std::min)(players, static_cast<std::int64_t>(std::numeric_limits<int>::max())));

    std::size_t index = (gauge_begin_ + gauge_size_) % gauges_.size();
    if (gauge_size_ == gauges_.size()) {
        index = gauge_begin_;
        gauge_begin_ = (gauge_begin_ + 1) % gauges_.size();
    }
    else {
        ++gauge_size_;
    }
    gauges_[index] = sample;
}

void StatisticsService::recordWorldGauges(int entities, int chunks)
{
    recordWorldGaugesAt(entities, chunks, last_observation_steady_ms_);
}

void StatisticsService::recordWorldGaugesAt(int entities, int chunks, std::int64_t steady_ms)
{
    if (!started_ || gauge_size_ == 0) {
        return;
    }
    steady_ms = (std::max)(steady_ms, last_observation_steady_ms_);
    std::size_t index = (gauge_begin_ + gauge_size_ - 1) % gauges_.size();
    gauges_[index].entities = entities;
    gauges_[index].chunks = chunks;
    gauges_[index].world_gauges_set = true;
    gauges_[index].steady_ms = (std::max)(gauges_[index].steady_ms, steady_ms);
    last_observation_steady_ms_ = (std::max)(last_observation_steady_ms_, steady_ms);

    if (steady_ms < start_steady_ms_ + MetricsHistory::kIntervalMs) {
        return;
    }
    const GaugeSample &sample = gauges_[index];
    metrics_history_.recordWorldInfo(unixTimeFor(steady_ms), sample.players, sample.entities, sample.chunks);
}

void StatisticsService::recordPlayerPing(const MetricsAverages &summary)
{
    recordPlayerPingAt(summary, last_observation_steady_ms_);
}

void StatisticsService::recordPlayerPingAt(const MetricsAverages &summary, std::int64_t steady_ms)
{
    if (!started_) {
        return;
    }
    steady_ms = (std::max)(steady_ms, last_observation_steady_ms_);
    last_observation_steady_ms_ = steady_ms;
    if (steady_ms < start_steady_ms_ + MetricsHistory::kIntervalMs) {
        return;
    }
    metrics_history_.recordPlayerPing(unixTimeFor(steady_ms), summary);
}

void StatisticsService::recordMetricsAt(std::int64_t steady_ms)
{
    if (!started_ || steady_ms < start_steady_ms_ + MetricsHistory::kIntervalMs) {
        return;
    }

    const std::int64_t timestamp_ms = unixTimeFor(steady_ms);
    const RollingValue tps = tpsFor(steady_ms, MetricsHistory::kIntervalMs);
    if (tps.present) {
        metrics_history_.recordTps(timestamp_ms, tps.value);
    }

    const DistributionValues tick_duration = msptForRecentSamples(kPlaceholderTickDuration1mSamples);
    if (tick_duration.present) {
        metrics_history_.recordTickDuration(timestamp_ms, {.mean = tick_duration.mean,
                                                           .max = tick_duration.max,
                                                           .min = tick_duration.min,
                                                           .median = tick_duration.median,
                                                           .percentile95 = tick_duration.percentile95});
    }

    const RollingValue process = cpuFor(steady_ms, MetricsHistory::kIntervalMs, true);
    if (process.present) {
        metrics_history_.recordCpuUsageProcess(timestamp_ms, process.value);
    }
    const RollingValue system = cpuFor(steady_ms, MetricsHistory::kIntervalMs, false);
    if (system.present) {
        metrics_history_.recordCpuUsageSystem(timestamp_ms, system.value);
    }
}

}  // namespace spark
