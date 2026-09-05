#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace spark::stable_entry_experiment {

#ifdef _WIN32

struct PermanentGatewayHandle {
    void *entry = nullptr;
    void *gateway = nullptr;
    void *original = nullptr;
    void *state = nullptr;
    std::size_t permanent_rx_bytes = 0;
    std::size_t permanent_rw_bytes = 0;
    std::uint64_t generation = 0;
    std::uint32_t stack_argument_count = 0;
};

// Current allocator signatures use at most one stack argument on Windows x64.
// The prototype rejects larger signatures rather than silently corrupting ABI
// state while forwarding through the process-lifetime call gateway.
bool installPermanentGateway(void *entry, std::uint32_t stack_argument_count, PermanentGatewayHandle &handle,
                             std::string &error);

// Reload path. Decode the stable rel32 entry, validate process-lifetime gateway
// identity/ABI/code/original metadata, then reuse the existing allocation.
bool discoverPermanentGateway(void *entry, PermanentGatewayHandle &handle, std::string &error);

// Admission stays closed until handler and generation are fully published.
bool bindPermanentGateway(PermanentGatewayHandle &handle, void *handler, std::uint64_t timeout_ms, std::string &error);

// Close admission, invalidate delayed admissions, drain every callback which
// can still be executing an unloadable handler, then clear the handler pointer.
bool detachPermanentGateway(PermanentGatewayHandle &handle, std::uint64_t timeout_ms, std::string &error);

[[nodiscard]] void *permanentGatewayOriginal(const PermanentGatewayHandle &handle) noexcept;
[[nodiscard]] void *permanentGatewayHandler(const PermanentGatewayHandle &handle) noexcept;
[[nodiscard]] std::uint64_t permanentGatewayActive(const PermanentGatewayHandle &handle) noexcept;
[[nodiscard]] std::uint64_t permanentGatewayGeneration(const PermanentGatewayHandle &handle) noexcept;
[[nodiscard]] bool permanentGatewayAdmissionOpen(const PermanentGatewayHandle &handle) noexcept;

#endif

}  // namespace spark::stable_entry_experiment
