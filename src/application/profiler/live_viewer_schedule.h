#ifndef SPARK_APPLICATION_PROFILER_LIVE_VIEWER_SCHEDULE_H
#define SPARK_APPLICATION_PROFILER_LIVE_VIEWER_SCHEDULE_H

#include <cstdint>

namespace spark {

struct LiveViewerDue {
    bool statistics = false;
    bool sampler = false;
    std::int32_t sampler_window = 0;
};

// Tracks live-viewer deadlines without performing work or advancing on due().
class LiveViewerSchedule {
public:
    static constexpr std::int64_t kStatisticsIntervalMs = 10'000;

    explicit LiveViewerSchedule(std::int32_t window_adjustment_ms) noexcept;

    void arm(std::int64_t now_ms) noexcept;
    void disarm() noexcept;
    [[nodiscard]] bool armed() const noexcept { return armed_; }

    [[nodiscard]] LiveViewerDue due(std::int64_t now_ms) noexcept;
    void commit(std::int64_t now_ms, const LiveViewerDue &due) noexcept;

private:
    bool advanceStatisticsDeadline(std::int64_t now_ms) noexcept;

    std::int32_t window_adjustment_ms_;
    bool armed_ = false;
    bool statistics_exhausted_ = false;
    bool sampler_exhausted_ = false;
    std::int64_t next_statistics_ms_ = 0;
    std::int64_t last_seen_ms_ = 0;
    std::int32_t sampler_window_ = 0;
    std::int32_t last_seen_window_ = 0;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_LIVE_VIEWER_SCHEDULE_H
