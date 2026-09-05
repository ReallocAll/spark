#include "native/alloc/windows_permanent_iat_gateway_registry_experiment.h"

#ifndef _WIN32
#error "windows_permanent_iat_gateway_registry_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using spark::permanent_iat_gateway_experiment::acquirePermanentIatGateway;
using spark::permanent_iat_gateway_experiment::bindPermanentIatGateway;
using spark::permanent_iat_gateway_experiment::detachPermanentIatGateway;
using spark::permanent_iat_gateway_experiment::permanentIatGatewayActive;
using spark::permanent_iat_gateway_experiment::permanentIatGatewayAdmissionOpen;
using spark::permanent_iat_gateway_experiment::permanentIatGatewayGeneration;
using spark::permanent_iat_gateway_experiment::PermanentIatGatewayHandle;
using spark::permanent_iat_gateway_experiment::permanentIatGatewayHandler;

namespace {

using TargetFn = std::uint64_t(__cdecl *)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

constexpr std::size_t kReloads = 1000;
constexpr std::uint64_t kTimeoutMs = 5000;
constexpr std::uint64_t kBias = 0x100000000ULL;

[[nodiscard]] std::uint64_t baseValue(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                      std::uint64_t e) noexcept
{
    return a + 3 * b + 5 * c + 7 * d + 11 * e;
}

extern "C" __declspec(noinline) std::uint64_t __cdecl originalFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                   std::uint64_t d, std::uint64_t e) noexcept
{
    return baseValue(a, b, c, d, e);
}

extern "C" __declspec(noinline) std::uint64_t __cdecl handlerFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                  std::uint64_t d, std::uint64_t e) noexcept
{
    return baseValue(a, b, c, d, e) + kBias;
}

[[nodiscard]] TargetFn functionAt(void *address) noexcept
{
    return reinterpret_cast<TargetFn>(address);
}

[[noreturn]] void fail(const char *reason, std::size_t cycle)
{
    std::fprintf(stderr, "stage=permanent-iat-gateway-registry failure=%s cycle=%zu\n", reason, cycle);
    std::fflush(stderr);
    std::abort();
}

}  // namespace

int main()
{
    std::fprintf(stderr, "stage=permanent-iat-gateway-registry begin reloads=%zu\n", kReloads);

    void *stable_gateway = nullptr;
    void *stable_state = nullptr;
    std::size_t stable_rx_bytes = 0;
    std::size_t stable_rw_bytes = 0;
    const std::uint64_t base = baseValue(1, 2, 3, 4, 5);
    std::string error;

    for (std::size_t cycle = 0; cycle < kReloads; ++cycle) {
        // Treat every iteration as a freshly loaded Spark image: there is no
        // retained handle/gateway pointer from the previous iteration. The only
        // stable identity supplied to acquisition is the allocator original.
        PermanentIatGatewayHandle handle;
        if (!acquirePermanentIatGateway(reinterpret_cast<void *>(&originalFive), 1, handle, error)) {
            std::fprintf(stderr, "stage=permanent-iat-gateway-registry acquire-failure cycle=%zu error=%s\n", cycle,
                         error.c_str());
            return 2;
        }
        if (cycle == 0) {
            stable_gateway = handle.gateway;
            stable_state = handle.state;
            stable_rx_bytes = handle.permanent_rx_bytes;
            stable_rw_bytes = handle.permanent_rw_bytes;
        }
        else if (handle.gateway != stable_gateway || handle.state != stable_state ||
                 handle.permanent_rx_bytes != stable_rx_bytes || handle.permanent_rw_bytes != stable_rw_bytes) {
            fail("gateway-identity-changed", cycle);
        }
        if (permanentIatGatewayAdmissionOpen(handle) || permanentIatGatewayHandler(handle) != nullptr) {
            fail("acquired-state-not-detached", cycle);
        }

        TargetFn gateway = functionAt(handle.gateway);
        if (gateway(1, 2, 3, 4, 5) != base) {
            fail("detached-pass-through", cycle);
        }
        if (!bindPermanentIatGateway(handle, reinterpret_cast<void *>(&handlerFive), kTimeoutMs, error)) {
            std::fprintf(stderr, "stage=permanent-iat-gateway-registry bind-failure cycle=%zu error=%s\n", cycle,
                         error.c_str());
            return 3;
        }
        if (gateway(1, 2, 3, 4, 5) != base + kBias) {
            fail("bound-handler-result", cycle);
        }
        if (!detachPermanentIatGateway(handle, kTimeoutMs, error)) {
            std::fprintf(stderr, "stage=permanent-iat-gateway-registry detach-failure cycle=%zu error=%s\n", cycle,
                         error.c_str());
            return 4;
        }
        if (gateway(1, 2, 3, 4, 5) != base || permanentIatGatewayAdmissionOpen(handle) ||
            permanentIatGatewayHandler(handle) != nullptr) {
            fail("post-detach-state", cycle);
        }

        // Drop all plugin-side state before the next acquisition.
        handle = {};

        if ((cycle + 1) % 100 == 0) {
            PermanentIatGatewayHandle observed;
            if (!acquirePermanentIatGateway(reinterpret_cast<void *>(&originalFive), 1, observed, error)) {
                fail("progress-reacquire", cycle);
            }
            std::fprintf(stderr,
                         "stage=permanent-iat-gateway-registry progress=%zu/%zu gateway=%p state=%p generation=%llu "
                         "active=%llu rx=%zu rw=%zu\n",
                         cycle + 1, kReloads, observed.gateway, observed.state,
                         static_cast<unsigned long long>(permanentIatGatewayGeneration(observed)),
                         static_cast<unsigned long long>(permanentIatGatewayActive(observed)),
                         observed.permanent_rx_bytes, observed.permanent_rw_bytes);
            std::fflush(stderr);
        }
    }

    PermanentIatGatewayHandle final_handle;
    if (!acquirePermanentIatGateway(reinterpret_cast<void *>(&originalFive), 1, final_handle, error)) {
        return 5;
    }
    if (final_handle.gateway != stable_gateway || final_handle.state != stable_state ||
        permanentIatGatewayAdmissionOpen(final_handle) || permanentIatGatewayHandler(final_handle) != nullptr ||
        permanentIatGatewayActive(final_handle) != 0) {
        fail("final-state", kReloads);
    }

    std::fprintf(stderr,
                 "stage=permanent-iat-gateway-registry pass reloads=%zu gateway=%p state=%p generation=%llu "
                 "permanent_rx_bytes=%zu permanent_rw_bytes=%zu\n",
                 kReloads, final_handle.gateway, final_handle.state,
                 static_cast<unsigned long long>(permanentIatGatewayGeneration(final_handle)),
                 final_handle.permanent_rx_bytes, final_handle.permanent_rw_bytes);
    return 0;
}
