#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace spark::permanent_iat_gateway_experiment {

#ifdef _WIN32

struct PermanentIatGatewayHandle {
    void *gateway = nullptr;
    void *original = nullptr;
    void *state = nullptr;
    std::size_t permanent_rx_bytes = 0;
    std::size_t permanent_rw_bytes = 0;
    std::uint64_t generation = 0;
    std::uint32_t stack_argument_count = 0;
};

// Build a process-lifetime call gateway completely off to the side. Nothing
// becomes reachable until the caller atomically publishes handle.gateway into
// an IAT/function-pointer slot. Successful gateway/state allocations are never
// reclaimed before process exit.
bool createPermanentIatGateway(void *original, std::uint32_t stack_argument_count, PermanentIatGatewayHandle &handle,
                               std::string &error);

// Reload path: recover and validate the process-lifetime state directly from a
// gateway pointer already stored in an IAT slot. No Spark DLL global is needed.
bool discoverPermanentIatGateway(void *gateway, PermanentIatGatewayHandle &handle, std::string &error);

bool bindPermanentIatGateway(PermanentIatGatewayHandle &handle, void *handler, std::uint64_t timeout_ms,
                             std::string &error);
bool detachPermanentIatGateway(PermanentIatGatewayHandle &handle, std::uint64_t timeout_ms, std::string &error);

[[nodiscard]] void *permanentIatGatewayOriginal(const PermanentIatGatewayHandle &handle) noexcept;
[[nodiscard]] void *permanentIatGatewayHandler(const PermanentIatGatewayHandle &handle) noexcept;
[[nodiscard]] std::uint64_t permanentIatGatewayActive(const PermanentIatGatewayHandle &handle) noexcept;
[[nodiscard]] std::uint64_t permanentIatGatewayGeneration(const PermanentIatGatewayHandle &handle) noexcept;
[[nodiscard]] bool permanentIatGatewayAdmissionOpen(const PermanentIatGatewayHandle &handle) noexcept;

#endif

}  // namespace spark::permanent_iat_gateway_experiment
