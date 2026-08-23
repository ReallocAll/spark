#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>

#include "core/util/monotonic_time.h"
#include "profiling_window.h"

namespace {

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
    using namespace spark::profiling_window;

    constexpr std::int64_t kTime = 1'700'000'012'345;
    constexpr std::int32_t kAdjustment = 12'345;
    const std::int32_t window = timeToWindow(kTime, kAdjustment);
    assert(timeToWindow(kTime + kSizeMs, kAdjustment) == window + 1);

    const std::int64_t start = windowStartTime(window, kAdjustment);
    const std::int64_t end = windowEndTime(window, kAdjustment);
    assert(start <= kTime && kTime < end);
    assert(timeToWindow(start, kAdjustment) == window);
    assert(timeToWindow(end - 1, kAdjustment) == window);
    assert(timeToWindow(end, kAdjustment) == window + 1);
}

void testAdjustmentAndPruning()
{
    using namespace spark::profiling_window;

    const std::int32_t adjustment = windowAdjustmentMs();
    assert(adjustment >= kAdjustmentMinMs && adjustment <= kAdjustmentMaxMs);
    for (int i = 0; i < 100; ++i) {
        assert(windowAdjustmentMs() == adjustment);
    }
    assert(!shouldPrune(40, 100));
    assert(!shouldPrune(41, 100));
    assert(shouldPrune(39, 100));
}

void testOverflowHandling()
{
    using namespace spark::profiling_window;

    assert(timeToWindow((std::numeric_limits<std::int64_t>::max)(), kAdjustmentMaxMs) ==
           (std::numeric_limits<std::int32_t>::max)());
    assert(timeToWindow((std::numeric_limits<std::int64_t>::min)(), kAdjustmentMinMs) ==
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
