#ifndef SPARK_CORE_UTIL_MONOTONIC_TIME_H
#define SPARK_CORE_UTIL_MONOTONIC_TIME_H

#include <cstdint>

namespace spark {

std::int64_t unixMillisFromAnchors(std::int64_t system_anchor_ms, std::int64_t steady_anchor_ms,
                                   std::int64_t steady_now_ms) noexcept;

std::int64_t monotonicUnixMillis() noexcept;

}  // namespace spark

#endif  // SPARK_CORE_UTIL_MONOTONIC_TIME_H
