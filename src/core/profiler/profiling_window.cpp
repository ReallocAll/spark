#include "profiling_window.h"

#include <chrono>
#include <cstdint>
#include <limits>

#include "core/util/monotonic_time.h"

namespace spark::profiling_window {
namespace {

std::uint64_t mix(std::uint64_t value) noexcept
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::int32_t chooseAdjustment() noexcept
{
    const auto steady = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto system = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    const std::uint64_t value = mix(steady ^ (system + 0x9e3779b97f4a7c15ULL));
    return static_cast<std::int32_t>(value % static_cast<std::uint64_t>(kSizeMs)) + kAdjustmentMinMs;
}

const std::int32_t KWindowAdjustmentMs = chooseAdjustment();

std::int32_t clampWindow(std::int64_t window) noexcept
{
    constexpr std::int64_t k_min = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int32_t>::max();
    if (window < k_min) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (window > k_max) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(window);
}

std::int64_t saturatingAdd(std::int64_t left, std::int64_t right) noexcept
{
    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();
    if (right > 0 && left > k_max - right) {
        return k_max;
    }
    if (right < 0 && left < k_min - right) {
        return k_min;
    }
    return left + right;
}

}  // namespace

std::int32_t timeToWindow(std::int64_t time_ms, std::int32_t adjustment_ms) noexcept
{
    const std::int64_t adjusted_time = saturatingAdd(time_ms, adjustment_ms);
    return clampWindow(adjusted_time / kSizeMs);
}

std::int64_t windowStartTime(std::int32_t window, std::int32_t adjustment_ms) noexcept
{
    return static_cast<std::int64_t>(window) * kSizeMs - adjustment_ms;
}

std::int64_t windowEndTime(std::int32_t window, std::int32_t adjustment_ms) noexcept
{
    return windowStartTime(window, adjustment_ms) + kSizeMs;
}

std::int32_t windowAdjustmentMs() noexcept
{
    return KWindowAdjustmentMs;
}

std::int32_t windowNow() noexcept
{
    return timeToWindow(monotonicUnixMillis(), KWindowAdjustmentMs);
}

bool shouldPrune(std::int32_t window, std::int32_t current_window) noexcept
{
    return static_cast<std::int64_t>(window) < static_cast<std::int64_t>(current_window) - kHistorySize;
}

}  // namespace spark::profiling_window
