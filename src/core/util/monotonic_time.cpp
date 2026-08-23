#include "core/util/monotonic_time.h"

#include <chrono>
#include <cstdint>
#include <limits>

namespace spark {
namespace {

using Milliseconds = std::chrono::milliseconds;

std::int64_t saturatingAdd(std::int64_t left, std::int64_t right) noexcept
{
    constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
    constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
    if (right > 0 && left > kMax - right) {
        return kMax;
    }
    if (right < 0 && left < kMin - right) {
        return kMin;
    }
    return left + right;
}

std::int64_t saturatingSubtract(std::int64_t left, std::int64_t right) noexcept
{
    constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
    constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
    if (right > 0 && left < kMin + right) {
        return kMin;
    }
    if (right < 0 && left > kMax + right) {
        return kMax;
    }
    return left - right;
}

std::int64_t saturatingDifference(std::int64_t left, std::int64_t right) noexcept
{
    return saturatingSubtract(left, right);
}

struct ClockAnchors {
    std::int64_t system_ms;
    std::int64_t steady_ms;
};

ClockAnchors captureAnchors() noexcept
{
    const auto steady = std::chrono::steady_clock::now();
    const auto system = std::chrono::system_clock::now();
    return {.system_ms = std::chrono::duration_cast<Milliseconds>(system.time_since_epoch()).count(),
            .steady_ms = std::chrono::duration_cast<Milliseconds>(steady.time_since_epoch()).count()};
}

const ClockAnchors kClockAnchors = captureAnchors();

}  // namespace

std::int64_t unixMillisFromAnchors(std::int64_t system_anchor_ms, std::int64_t steady_anchor_ms,
                                   std::int64_t steady_now_ms) noexcept
{
    return saturatingAdd(system_anchor_ms, saturatingDifference(steady_now_ms, steady_anchor_ms));
}

std::int64_t monotonicUnixMillis() noexcept
{
    const auto steady_now =
        std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return unixMillisFromAnchors(kClockAnchors.system_ms, kClockAnchors.steady_ms, steady_now);
}

}  // namespace spark
