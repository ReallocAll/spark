#ifndef ENDSTONE_SPARK_PROFILING_WINDOW_H
#define ENDSTONE_SPARK_PROFILING_WINDOW_H

#include <cstdint>

namespace spark::profiling_window {

inline constexpr std::int32_t kSizeSeconds = 60;
inline constexpr std::int64_t kSizeMs = static_cast<std::int64_t>(kSizeSeconds) * 1000;
inline constexpr std::int32_t kHistorySize = 60;
inline constexpr std::int64_t kHistoryMs = static_cast<std::int64_t>(kHistorySize) * kSizeMs;
inline constexpr std::int32_t kAdjustmentMinMs = -static_cast<std::int32_t>(kSizeMs / 2);
inline constexpr std::int32_t kAdjustmentMaxMs = static_cast<std::int32_t>(kSizeMs / 2) - 1;

[[nodiscard]] std::int32_t timeToWindow(std::int64_t time_ms, std::int32_t adjustment_ms) noexcept;
[[nodiscard]] std::int64_t windowStartTime(std::int32_t window, std::int32_t adjustment_ms) noexcept;
[[nodiscard]] std::int64_t windowEndTime(std::int32_t window, std::int32_t adjustment_ms) noexcept;
[[nodiscard]] std::int32_t windowAdjustmentMs() noexcept;
[[nodiscard]] std::int32_t windowNow() noexcept;
[[nodiscard]] bool shouldPrune(std::int32_t window, std::int32_t current_window) noexcept;

}  // namespace spark::profiling_window

#endif  // ENDSTONE_SPARK_PROFILING_WINDOW_H
