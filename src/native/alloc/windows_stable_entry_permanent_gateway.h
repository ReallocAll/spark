#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace spark::stable_entry_experiment {

#ifdef _WIN32

enum class PermanentGatewayArity : std::uint32_t {
    UpToFourIntegerArgs = 1,
    FiveIntegerArgs = 2,
};

struct PermanentGatewayFootprint {
    std::size_t island_reserved = 0;
    std::size_t island_committed = 0;
    std::size_t trampoline_reserved_committed = 0;
};

// Non-owning control handle for a process-lifetime stable-entry gateway.
//
// The executable island, RW dispatch state and original trampoline are
// intentionally never reclaimed after the allocator entry publishes them.
// Destroying this C++ handle does not affect gateway lifetime. This is the core
// unload-safety property under experiment: Spark-owned state can disappear while
// allocator calls continue through permanent pass-through code.
class PermanentGateway {
public:
    PermanentGateway() noexcept = default;

    // If entry already contains a validated Spark permanent-gateway signature,
    // rediscover and reuse it. Otherwise create a new closed/pass-through island
    // and atomically publish one aligned 8-byte rel32 entry transaction.
    static bool installOrRediscover(void *entry, PermanentGatewayArity arity, PermanentGateway &result,
                                    std::string &error);

    // Publish a Spark-owned handler before opening admission. The permanent
    // gateway acquires an active token before it can load/call this pointer.
    bool attach(void *handler, std::string &error) noexcept;

    // Close admission + advance generation, drain callbacks which already
    // acquired a token, then clear the Spark-owned handler pointer. No permanent
    // executable/data allocation is reclaimed.
    bool detach(std::uint64_t timeout_ms, std::string &error) noexcept;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr && gateway_ != nullptr; }
    [[nodiscard]] bool admissionClosed() const noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] std::uint32_t activeCount() const noexcept;
    [[nodiscard]] std::uint32_t generation() const noexcept;
    [[nodiscard]] void *handler() const noexcept;
    [[nodiscard]] void *entry() const noexcept { return entry_; }
    [[nodiscard]] void *gateway() const noexcept { return gateway_; }
    [[nodiscard]] void *state() const noexcept { return state_; }
    [[nodiscard]] void *originalTrampoline() const noexcept { return original_trampoline_; }
    [[nodiscard]] PermanentGatewayArity arity() const noexcept { return arity_; }
    [[nodiscard]] PermanentGatewayFootprint footprint() const noexcept { return footprint_; }

    // Used by unload tests to prove that the permanent state no longer contains
    // an address inside the unloadable Spark image after detach.
    [[nodiscard]] bool containsAddressInRange(std::uintptr_t begin, std::uintptr_t end) const noexcept;

private:
    void *entry_ = nullptr;
    void *gateway_ = nullptr;
    void *state_ = nullptr;
    void *original_trampoline_ = nullptr;
    PermanentGatewayArity arity_ = PermanentGatewayArity::UpToFourIntegerArgs;
    PermanentGatewayFootprint footprint_{};
};

#endif

}  // namespace spark::stable_entry_experiment
