#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>

#include "core/util/monotonic_time.h"
#include "profiling_window.h"

namespace {

namespace profiling_window = spark::profiling_window;

void testAnchorConversion()
{
    assert(spark::unixMillisFromAnchors(1'700'000'000'000, 10'000, 10'123) == 1'700'000'000'123);
    assert(spark::unixMillisFromAnchors(1'000, 10'123, 10'000) == 877);
    assert(spark::unixMillisFromAnchors((std::numeric_limits<std::int64_t>::max)(), 0, 1) ==
           (std::numeric_limits<std::int64_t>::max)());
    assert(spark::unixMillisFromAnchors((std::numeric_limits<std::int64_t>::min)(), 0, -1) ==
           (std::numeric_limits<std::int64_t>::min)());
}

void testMonotonicUnixMillis()
{
    const auto before =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::int64_t first = spark::monotonicUnixMillis();
    const std::int64_t second = spark::monotonicUnixMillis();
    const auto after =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    assert(second >= first);
    assert(first >= before - 1'000);
    assert(first <= after + 1'000);
}

void testProfilingWindows()
{
    constexpr std::int64_t k_time = 1'700'000'012'345;
    constexpr std::int32_t k_adjustment = 12'345;
    const std::int32_t window = profiling_window::timeToWindow(k_time, k_adjustment);
    assert(profiling_window::timeToWindow(k_time + profiling_window::kSizeMs, k_adjustment) == window + 1);

    const std::int64_t start = profiling_window::windowStartTime(window, k_adjustment);
    const std::int64_t end = profiling_window::windowEndTime(window, k_adjustment);
    assert(start <= k_time && k_time < end);
    assert(profiling_window::timeToWindow(start, k_adjustment) == window);
    assert(profiling_window::timeToWindow(end - 1, k_adjustment) == window);
    assert(profiling_window::timeToWindow(end, k_adjustment) == window + 1);
}

void testAdjustmentAndPruning()
{
    const std::int32_t adjustment = profiling_window::windowAdjustmentMs();
    assert(adjustment >= profiling_window::kAdjustmentMinMs && adjustment <= profiling_window::kAdjustmentMaxMs);
    for (int i = 0; i < 100; ++i) {
        assert(profiling_window::windowAdjustmentMs() == adjustment);
    }
    assert(!profiling_window::shouldPrune(40, 100));
    assert(!profiling_window::shouldPrune(41, 100));
    assert(profiling_window::shouldPrune(39, 100));
}

void testOverflowHandling()
{
    assert(profiling_window::timeToWindow((std::numeric_limits<std::int64_t>::max)(),
                                          profiling_window::kAdjustmentMaxMs) ==
           (std::numeric_limits<std::int32_t>::max)());
    assert(profiling_window::timeToWindow((std::numeric_limits<std::int64_t>::min)(),
                                          profiling_window::kAdjustmentMinMs) ==
           (std::numeric_limits<std::int32_t>::min)());
}

}  // namespace

int main()
{
    testAnchorConversion();
    testMonotonicUnixMillis();
    testProfilingWindows();
    testAdjustmentAndPruning();
    testOverflowHandling();
    return 0;
}
