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
};

// First-install path. This never restores the entry or reclaims the generated
// gateway/trampoline after publication. On any unsupported condition before
// publication it fails closed and releases temporary resources.
bool installPermanentGateway(void *entry, PermanentGatewayHandle &handle, std::string &error);

// Reload path. Decode the stable rel32 entry, validate the process-lifetime
// gateway signature, ABI, hashes and backing memory, then reuse it.
bool discoverPermanentGateway(void *entry, PermanentGatewayHandle &handle, std::string &error);

// Bind one unloadable handler generation. Admission remains closed until the
// handler pointer and generation are fully published.
bool bindPermanentGateway(PermanentGatewayHandle &handle, void *handler, std::uint64_t timeout_ms,
                          std::string &error);

// Close admission, invalidate cached generations, drain admitted callbacks and
// remove the only unloadable-image address from permanent state.
bool detachPermanentGateway(PermanentGatewayHandle &handle, std::uint64_t timeout_ms, std::string &error);

[[nodiscard]] void *permanentGatewayOriginal(const PermanentGatewayHandle &handle) noexcept;
void completePermanentGatewayHandlerCall(const PermanentGatewayHandle &handle) noexcept;
[[nodiscard]] void *permanentGatewayHandler(const PermanentGatewayHandle &handle) noexcept;
[[nodiscard]] std::uint64_t permanentGatewayActive(const PermanentGatewayHandle &handle) noexcept;
[[nodiscard]] std::uint64_t permanentGatewayGeneration(const PermanentGatewayHandle &handle) noexcept;
[[nodiscard]] bool permanentGatewayAdmissionOpen(const PermanentGatewayHandle &handle) noexcept;

#endif

}  // namespace spark::stable_entry_experiment
