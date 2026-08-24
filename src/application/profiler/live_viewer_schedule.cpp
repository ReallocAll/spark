#include "application/profiler/live_viewer_schedule.h"

#include <limits>

#include "profiling_window.h"

namespace spark {

LiveViewerSchedule::LiveViewerSchedule(std::int32_t window_adjustment_ms) noexcept
    : window_adjustment_ms_(window_adjustment_ms)
{
}

void LiveViewerSchedule::arm(std::int64_t now_ms) noexcept
{
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();
    armed_ = true;
    statistics_exhausted_ = now_ms > k_max - kStatisticsIntervalMs;
    next_statistics_ms_ = statistics_exhausted_ ? 0 : now_ms + kStatisticsIntervalMs;
    last_seen_ms_ = now_ms;
    sampler_window_ = profiling_window::timeToWindow(now_ms, window_adjustment_ms_);
    sampler_exhausted_ = sampler_window_ == std::numeric_limits<std::int32_t>::max();
    last_seen_window_ = sampler_window_;
}

void LiveViewerSchedule::disarm() noexcept
{
    armed_ = false;
}

LiveViewerDue LiveViewerSchedule::due(std::int64_t now_ms) noexcept
{
    LiveViewerDue result;
    if (!armed_) {
        return result;
    }

    const std::int32_t current_window = profiling_window::timeToWindow(now_ms, window_adjustment_ms_);
    const bool time_went_backward = now_ms < last_seen_ms_;
    const bool window_went_backward = current_window < last_seen_window_;
    if (time_went_backward || window_went_backward) {
        return result;
    }
    last_seen_ms_ = now_ms;
    last_seen_window_ = current_window;

    result.statistics = !statistics_exhausted_ && now_ms >= next_statistics_ms_;
    result.sampler = !sampler_exhausted_ && current_window > sampler_window_;
    result.sampler_window = current_window;
    return result;
}

void LiveViewerSchedule::commit(std::int64_t now_ms, const LiveViewerDue &due_value) noexcept
{
    if (!armed_) {
        return;
    }
    if (due_value.statistics && !statistics_exhausted_) {
        statistics_exhausted_ = !advanceStatisticsDeadline(now_ms);
    }
    if (due_value.sampler && !sampler_exhausted_) {
        sampler_window_ = due_value.sampler_window;
        if (sampler_window_ == std::numeric_limits<std::int32_t>::max()) {
            sampler_exhausted_ = true;
        }
    }
}

bool LiveViewerSchedule::advanceStatisticsDeadline(std::int64_t now_ms) noexcept
{
    if (now_ms < next_statistics_ms_) {
        return true;
    }
    const auto interval = static_cast<std::uint64_t>(kStatisticsIntervalMs);
    const auto elapsed = static_cast<std::uint64_t>(now_ms) - static_cast<std::uint64_t>(next_statistics_ms_);
    const auto intervals = elapsed / interval + 1;
    const auto remaining = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                           static_cast<std::uint64_t>(next_statistics_ms_);
    if (intervals > remaining / interval) {
        return false;
    }
    const auto advance = intervals * interval;
    next_statistics_ms_ = static_cast<std::int64_t>(static_cast<std::uint64_t>(next_statistics_ms_) + advance);
    return true;
}

}  // namespace spark
