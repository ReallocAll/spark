#include <cassert>
#include <cstdint>
#include <limits>

#include "application/profiler/live_viewer_schedule.h"
#include "profiling_window.h"

namespace {

void testStatisticsCadenceAndCommit()
{
    spark::LiveViewerSchedule schedule(0);
    schedule.arm(0);
    assert(!schedule.due(9'999).statistics);
    const auto due = schedule.due(10'000);
    assert(due.statistics);
    assert(schedule.due(10'000).statistics);
    schedule.commit(10'000, due);
    assert(!schedule.due(10'001).statistics);
    assert(schedule.due(20'000).statistics);
}

void testWindowTransitionAndDelayedCoalescing()
{
    constexpr std::int32_t adjustment = 1'234;
    spark::LiveViewerSchedule schedule(adjustment);
    const std::int64_t boundary = spark::profiling_window::windowStartTime(42, adjustment);
    schedule.arm(boundary - 1);
    assert(!schedule.due(boundary - 1).sampler);
    const auto sampler_due = schedule.due(boundary);
    assert(sampler_due.sampler);
    assert(sampler_due.sampler_window == 42);
    schedule.commit(boundary, sampler_due);
    assert(!schedule.due(boundary + 1).sampler);

    schedule.arm(0);
    const auto delayed = schedule.due(35'000);
    assert(delayed.statistics);
    schedule.commit(35'000, delayed);
    assert(!schedule.due(35'001).statistics);
    assert(schedule.due(40'000).statistics);
}

void testBackwardTimeAndDisarm()
{
    spark::LiveViewerSchedule schedule(0);
    schedule.arm(0);
    assert(schedule.due(20'000).statistics);
    assert(!schedule.due(10'000).statistics);
    assert(!schedule.due(15'000).statistics);
    assert(schedule.due(20'000).statistics);
    schedule.disarm();
    assert(!schedule.armed());
    assert(!schedule.due(100'000).statistics);
}

void testSaturation()
{
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    spark::LiveViewerSchedule schedule(0);
    schedule.arm(max - 5'000);
    assert(!schedule.due(max).statistics);

    schedule.arm(max - spark::LiveViewerSchedule::kStatisticsIntervalMs);
    const auto due = schedule.due(max);
    assert(due.statistics);
    schedule.commit(max, due);
    assert(!schedule.due(max).statistics);
}

}  // namespace

int main()
{
    testStatisticsCadenceAndCommit();
    testWindowTransitionAndDelayedCoalescing();
    testBackwardTimeAndDisarm();
    testSaturation();
    return 0;
}
