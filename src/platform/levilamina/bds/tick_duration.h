#pragma once

#include <cstdint>
#include <optional>

namespace spark::levilamina::bds {

// Reads the completed server tick duration exposed by the pinned BDS runtime.
[[nodiscard]] std::optional<double> readServerTickMilliseconds() noexcept;

// Returns the host thread identifier used by Spark's main-thread profiler path.
[[nodiscard]] std::uint64_t currentThreadId() noexcept;

}  // namespace spark::levilamina::bds
