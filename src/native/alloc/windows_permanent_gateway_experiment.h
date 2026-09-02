#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace spark::permanent_gateway_experiment {

constexpr std::uint32_t kGatewayAbiVersion = 1;
constexpr std::size_t kGatewayCodeOffset = 64;

struct GatewayFootprint {
    std::size_t island_committed = 0;
    std::size_t state_committed = 0;
    std::size_t trampoline_committed = 0;
};

class PermanentGateway {
public:
    PermanentGateway() = default;

    PermanentGateway(const PermanentGateway &) = delete;
    PermanentGateway &operator=(const PermanentGateway &) = delete;

    // Installs the permanent entry patch exactly once, or rediscovers an already
    // installed compatible process-lifetime gateway from the allocator entry.
    // stack_argument_count is currently restricted to 0 or 1 x64 stack arguments.
    static bool installOrRediscover(void *entry, std::uint32_t stack_argument_count, PermanentGateway &gateway,
                                    bool &created, std::string &error);

    // handler must point into the currently loaded Spark/handler image. The
    // owner cookie is generation-local and is cleared only after close+drain.
    bool attach(void *handler, std::uint64_t owner_cookie, std::string &error) noexcept;
    bool detach(std::uint64_t owner_cookie, std::uint64_t timeout_ms, std::string &error) noexcept;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr && island_ != nullptr; }
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] std::uint32_t activeCount() const noexcept;
    [[nodiscard]] std::uint32_t generation() const noexcept;
    [[nodiscard]] void *handlerAddress() const noexcept;
    [[nodiscard]] void *originalTrampoline() const noexcept;
    [[nodiscard]] void *gatewayEntry() const noexcept;
    [[nodiscard]] void *islandBase() const noexcept { return island_; }
    [[nodiscard]] void *stateBase() const noexcept { return state_; }
    [[nodiscard]] std::size_t gatewayCodeSize() const noexcept { return code_size_; }
    [[nodiscard]] GatewayFootprint footprint() const noexcept;

private:
    void *entry_ = nullptr;
    void *island_ = nullptr;
    void *state_ = nullptr;
    std::size_t code_size_ = 0;
};

}  // namespace spark::permanent_gateway_experiment
