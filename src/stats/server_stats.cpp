#include "stats/server_stats.h"

#include <algorithm>
#include <cmath>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <time.h>
#endif

namespace spark {
namespace {

std::size_t boundedCount(std::size_t size, std::size_t requested)
{
    return std::min(size, requested);
}

}  // namespace

ServerStats::ServerStats() : start_time_(std::chrono::system_clock::now()) {}

bool ServerStats::readCurrentThreadCpuMilliseconds(double &value_ms)
{
#if defined(_WIN32)
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (GetThreadTimes(GetCurrentThread(), &creation_time, &exit_time, &kernel_time, &user_time) == 0) {
        return false;
    }

    ULARGE_INTEGER kernel{};
    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    ULARGE_INTEGER user{};
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;

    // FILETIME units are 100 ns.
    value_ms = static_cast<double>(kernel.QuadPart + user.QuadPart) / 10000.0;
    return std::isfinite(value_ms);
#elif defined(__linux__) || defined(__APPLE__)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
        return false;
    }
    value_ms = static_cast<double>(value.tv_sec) * 1000.0 +
               static_cast<double>(value.tv_nsec) / 1000000.0;
    return std::isfinite(value_ms);
#else
    (void)value_ms;
    return false;
#endif
}

double ServerStats::estimateMspt(double interval_ms, double cpu_delta_ms, bool cpu_delta_valid)
{
    if (!std::isfinite(interval_ms) || interval_ms <= 0.0) {
        return 0.0;
    }

    // Without thread CPU accounting we can only identify the part that exceeds
    // the normal 50 ms pacing budget. On supported Windows/Linux builds the CPU
    // path should normally be available.
    if (!cpu_delta_valid || !std::isfinite(cpu_delta_ms) || cpu_delta_ms < 0.0) {
        return interval_ms >= kOverrunBlendEndMs
                   ? interval_ms
                   : std::max(0.0, interval_ms - kTargetTickIntervalMs);
    }

    const double cpu_ms = std::clamp(cpu_delta_ms, 0.0, interval_ms);
    if (interval_ms <= kOverrunBlendStartMs) {
        return cpu_ms;
    }
    if (interval_ms >= kOverrunBlendEndMs) {
        // At this point the server is clearly missing its 20 TPS pacing budget;
        // the same-phase callback period is the best available wall-time MSPT.
        return std::max(cpu_ms, interval_ms);
    }

    // Smoothly transition through the scheduler/jitter zone rather than jumping
    // from CPU time to wall time at a single threshold.
    double alpha = (interval_ms - kOverrunBlendStartMs) /
                   (kOverrunBlendEndMs - kOverrunBlendStartMs);
    alpha = std::clamp(alpha, 0.0, 1.0);
    alpha = alpha * alpha * (3.0 - 2.0 * alpha);  // smoothstep
    return cpu_ms + (interval_ms - cpu_ms) * alpha;
}

void ServerStats::onTick()
{
    const auto now = std::chrono::steady_clock::now();
    double current_cpu_ms = 0.0;
    const bool current_cpu_valid = readCurrentThreadCpuMilliseconds(current_cpu_ms);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_previous_tick_) {
        previous_tick_time_ = now;
        previous_thread_cpu_ms_ = current_cpu_ms;
        previous_thread_cpu_valid_ = current_cpu_valid;
        has_previous_tick_ = true;
        return;
    }

    const double interval_ms =
        std::chrono::duration<double, std::milli>(now - previous_tick_time_).count();
    previous_tick_time_ = now;

    bool cpu_delta_valid = false;
    double cpu_delta_ms = 0.0;
    if (current_cpu_valid && previous_thread_cpu_valid_) {
        cpu_delta_ms = current_cpu_ms - previous_thread_cpu_ms_;
        cpu_delta_valid = std::isfinite(cpu_delta_ms) && cpu_delta_ms >= 0.0;
    }
    previous_thread_cpu_ms_ = current_cpu_ms;
    previous_thread_cpu_valid_ = current_cpu_valid;

    if (!std::isfinite(interval_ms) || interval_ms <= 0.0) {
        return;
    }

    const double estimated_mspt_ms = estimateMspt(interval_ms, cpu_delta_ms, cpu_delta_valid);
    samples_.push_back({interval_ms, estimated_mspt_ms});
    if (samples_.size() > kAverageWindowSamples) {
        samples_.pop_front();
    }
    cpu_time_observed_ = cpu_time_observed_ || cpu_delta_valid;
}

float ServerStats::tpsForLast(const std::deque<TickSample> &samples, std::size_t count)
{
    count = boundedCount(samples.size(), count);
    if (count == 0) {
        return kTargetTps;
    }

    double interval_sum_ms = 0.0;
    const std::size_t first = samples.size() - count;
    for (std::size_t i = first; i < samples.size(); ++i) {
        interval_sum_ms += samples[i].interval_ms;
    }
    if (!std::isfinite(interval_sum_ms) || interval_sum_ms <= 0.0) {
        return kTargetTps;
    }

    const double tps = static_cast<double>(count) * 1000.0 / interval_sum_ms;
    return std::clamp(static_cast<float>(tps), 0.0f, kTargetTps);
}

float ServerStats::meanMsptForLast(const std::deque<TickSample> &samples, std::size_t count)
{
    count = boundedCount(samples.size(), count);
    if (count == 0) {
        return 0.0f;
    }

    double sum_ms = 0.0;
    const std::size_t first = samples.size() - count;
    for (std::size_t i = first; i < samples.size(); ++i) {
        sum_ms += samples[i].estimated_mspt_ms;
    }
    return static_cast<float>(sum_ms / static_cast<double>(count));
}

float ServerStats::getCurrentTicksPerSecond() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tpsForLast(samples_, kCurrentWindowSamples);
}

float ServerStats::getAverageTicksPerSecond() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tpsForLast(samples_, kAverageWindowSamples);
}

float ServerStats::getLastMillisecondsPerTick() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.empty() ? 0.0f : static_cast<float>(samples_.back().estimated_mspt_ms);
}

float ServerStats::getCurrentMillisecondsPerTick() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return meanMsptForLast(samples_, kCurrentWindowSamples);
}

float ServerStats::getAverageMillisecondsPerTick() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return meanMsptForLast(samples_, kAverageWindowSamples);
}

float ServerStats::getMaximumMillisecondsPerTick() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    float maximum = 0.0f;
    for (const TickSample &sample : samples_) {
        maximum = std::max(maximum, static_cast<float>(sample.estimated_mspt_ms));
    }
    return maximum;
}

std::chrono::system_clock::time_point ServerStats::getStartTime() const
{
    return start_time_;
}

bool ServerStats::hasIntervalData() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !samples_.empty();
}

bool ServerStats::usesThreadCpuTime() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return cpu_time_observed_;
}

}  // namespace spark
