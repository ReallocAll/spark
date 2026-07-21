#ifndef ENDSTONE_SPARK_SERVER_STATS_H
#define ENDSTONE_SPARK_SERVER_STATS_H

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>

namespace spark {

/**
 * Tracks TPS and provides the best non-hook MSPT estimate available on
 * Endstone v0.5.0.
 *
 * TPS is measured from the wall-clock interval between consecutive scheduler
 * callbacks. MSPT is estimated from current-main-thread CPU time while the
 * server is keeping up, and transitions to wall-clock tick period when the
 * server is clearly overrunning the 50 ms tick budget. This cannot account
 * perfectly for blocking waits inside a healthy (<50 ms) tick, but is much
 * closer to true server work time than treating every callback interval as
 * 50 ms.
 */
class ServerStats {
public:
    ServerStats();

    /** Records one main-thread server-tick callback. */
    void onTick();

    /** TPS over the recent approximately-five-second window. */
    [[nodiscard]] float getCurrentTicksPerSecond() const;

    /** TPS over the recent approximately-one-minute window. */
    [[nodiscard]] float getAverageTicksPerSecond() const;

    /** Latest per-tick hybrid MSPT estimate, used by tick filtering/monitoring. */
    [[nodiscard]] float getLastMillisecondsPerTick() const;

    /** Mean estimated MSPT over the recent approximately-five-second window. */
    [[nodiscard]] float getCurrentMillisecondsPerTick() const;

    /** Mean estimated MSPT over the recent approximately-one-minute window. */
    [[nodiscard]] float getAverageMillisecondsPerTick() const;

    /** Maximum estimated MSPT over the recent approximately-one-minute window. */
    [[nodiscard]] float getMaximumMillisecondsPerTick() const;

    /** Time when the plugin-side statistics tracker was constructed. */
    [[nodiscard]] std::chrono::system_clock::time_point getStartTime() const;

    /** Whether at least one complete tick interval has been observed. */
    [[nodiscard]] bool hasIntervalData() const;

    /** Whether native current-thread CPU accounting has produced usable data. */
    [[nodiscard]] bool usesThreadCpuTime() const;

private:
    struct TickSample {
        double interval_ms = 0.0;
        double estimated_mspt_ms = 0.0;
    };

    static constexpr std::size_t kCurrentWindowSamples = 100;
    static constexpr std::size_t kAverageWindowSamples = 1200;
    static constexpr double kTargetTickIntervalMs = 50.0;
    static constexpr double kOverrunBlendStartMs = 50.5;
    static constexpr double kOverrunBlendEndMs = 55.0;
    static constexpr float kTargetTps = 20.0f;

    [[nodiscard]] static bool readCurrentThreadCpuMilliseconds(double &value_ms);
    [[nodiscard]] static double estimateMspt(double interval_ms, double cpu_delta_ms,
                                             bool cpu_delta_valid);
    [[nodiscard]] static float tpsForLast(const std::deque<TickSample> &samples,
                                          std::size_t count);
    [[nodiscard]] static float meanMsptForLast(const std::deque<TickSample> &samples,
                                               std::size_t count);

    std::chrono::system_clock::time_point start_time_;
    mutable std::mutex mutex_;
    std::deque<TickSample> samples_;
    std::chrono::steady_clock::time_point previous_tick_time_{};
    double previous_thread_cpu_ms_ = 0.0;
    bool has_previous_tick_ = false;
    bool previous_thread_cpu_valid_ = false;
    bool cpu_time_observed_ = false;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_SERVER_STATS_H
