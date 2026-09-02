#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::PermanentGatewayHandle;
using spark::stable_entry_experiment::bindPermanentGateway;
using spark::stable_entry_experiment::completePermanentGatewayHandlerCall;
using spark::stable_entry_experiment::detachPermanentGateway;
using spark::stable_entry_experiment::discoverPermanentGateway;
using spark::stable_entry_experiment::installPermanentGateway;
using spark::stable_entry_experiment::permanentGatewayActive;
using spark::stable_entry_experiment::permanentGatewayAdmissionOpen;
using spark::stable_entry_experiment::permanentGatewayGeneration;
using spark::stable_entry_experiment::permanentGatewayHandler;
using spark::stable_entry_experiment::permanentGatewayOriginal;

namespace {

using SyntheticFn = int(__cdecl *)(int);

constexpr std::size_t kWorkers = 8;
constexpr std::size_t kCycles = 1000;
constexpr std::uint64_t kTimeoutMs = 5000;

PermanentGatewayHandle g_gateway;
std::atomic<std::uint64_t> g_handler_calls{0};
std::atomic<std::uint64_t> g_worker_calls{0};

extern "C" __declspec(noinline) int __cdecl syntheticPermanentHandler(int value) noexcept
{
    auto *original = reinterpret_cast<SyntheticFn>(permanentGatewayOriginal(g_gateway));
    if (original == nullptr) {
        std::abort();
    }
    const int result = original(value);
    g_handler_calls.fetch_add(1, std::memory_order_relaxed);
    completePermanentGatewayHandlerCall(g_gateway);
    return result;
}

class ExecutableSyntheticFunction {
public:
    ExecutableSyntheticFunction()
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(page_ != nullptr);
        assert((reinterpret_cast<std::uintptr_t>(page_) & 7U) == 0);
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(page_, code.data(), code.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, code.size()) != FALSE);
        DWORD old_protection = 0;
        assert(::VirtualProtect(page_, 64 * 1024, PAGE_EXECUTE_READ, &old_protection) != FALSE);
    }

    ~ExecutableSyntheticFunction()
    {
        // The stable entry intentionally still points at process-lifetime
        // gateway code. The target page is reclaimed only as the test process
        // exits, after all worker threads have joined.
        if (page_ != nullptr) {
            (void)::VirtualFree(page_, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(page_); }
    [[nodiscard]] void *address() const noexcept { return page_; }

private:
    std::uint8_t *page_ = nullptr;
};

void assertRediscovery(void *entry, void *expected_gateway, void *expected_state, void *expected_original,
                       std::uint64_t &rediscoveries)
{
    PermanentGatewayHandle discovered;
    std::string error;
    if (!discoverPermanentGateway(entry, discovered, error)) {
        std::cerr << "permanent-gateway rediscovery failed: " << error << '\n';
        std::abort();
    }
    assert(discovered.gateway == expected_gateway);
    assert(discovered.state == expected_state);
    assert(discovered.original == expected_original);
    assert(discovered.permanent_rx_bytes == g_gateway.permanent_rx_bytes);
    assert(discovered.permanent_rw_bytes == g_gateway.permanent_rw_bytes);
    ++rediscoveries;
}

}  // namespace

int main()
{
    std::cerr << "stage=permanent-gateway begin\n";
    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    assert(function(41) == 42);

    std::string error;
    if (!installPermanentGateway(target.address(), g_gateway, error)) {
        std::cerr << "permanent-gateway install failed: " << error << '\n';
        return 2;
    }
    assert(g_gateway.entry == target.address());
    assert(g_gateway.gateway != nullptr);
    assert(g_gateway.original != nullptr);
    assert(g_gateway.state != nullptr);
    assert(g_gateway.permanent_rx_bytes != 0);
    assert(g_gateway.permanent_rw_bytes != 0);
    assert(!permanentGatewayAdmissionOpen(g_gateway));
    assert(permanentGatewayHandler(g_gateway) == nullptr);
    assert(permanentGatewayActive(g_gateway) == 0);
    assert(function(9) == 10);

    const void *stable_gateway = g_gateway.gateway;
    const void *stable_state = g_gateway.state;
    const void *stable_original = g_gateway.original;
    std::uint64_t rediscoveries = 0;
    assertRediscovery(target.address(), const_cast<void *>(stable_gateway), const_cast<void *>(stable_state),
                      const_cast<void *>(stable_original), rediscoveries);

    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = static_cast<int>(worker + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const int result = function(value);
                if (result != value + 1) {
                    std::cerr << "permanent-gateway mismatch worker=" << worker
                              << " generation=" << permanentGatewayGeneration(g_gateway)
                              << " active=" << permanentGatewayActive(g_gateway)
                              << " value=" << value << " result=" << result << '\n';
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                g_worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        if (!bindPermanentGateway(g_gateway, reinterpret_cast<void *>(&syntheticPermanentHandler), kTimeoutMs,
                                  error)) {
            std::cerr << "permanent-gateway bind failed cycle=" << cycle << " error=" << error << '\n';
            std::abort();
        }
        assert(permanentGatewayAdmissionOpen(g_gateway));
        assert(permanentGatewayHandler(g_gateway) == reinterpret_cast<void *>(&syntheticPermanentHandler));

        for (int value = 0; value < 32; ++value) {
            assert(function(value) == value + 1);
        }

        // Create a fresh thread every generation and overlap its calls with the
        // detach transition. This exercises cached-generation and admission
        // races rather than only a fixed worker pool.
        std::thread churn([&] {
            for (int value = 0; value < 256; ++value) {
                if (function(value) != value + 1) {
                    std::abort();
                }
            }
        });

        if (!detachPermanentGateway(g_gateway, kTimeoutMs, error)) {
            std::cerr << "permanent-gateway detach failed cycle=" << cycle << " error=" << error << '\n';
            std::abort();
        }
        churn.join();

        assert(!permanentGatewayAdmissionOpen(g_gateway));
        assert(permanentGatewayHandler(g_gateway) == nullptr);
        assert(permanentGatewayActive(g_gateway) == 0);
        assert(function(9) == 10);
        assert(g_gateway.gateway == stable_gateway);
        assert(g_gateway.state == stable_state);
        assert(g_gateway.original == stable_original);

        assertRediscovery(target.address(), const_cast<void *>(stable_gateway), const_cast<void *>(stable_state),
                          const_cast<void *>(stable_original), rediscoveries);
        if ((cycle + 1) % 25 == 0) {
            std::cerr << "stage=permanent-gateway progress=" << (cycle + 1) << '/' << kCycles
                      << " generation=" << permanentGatewayGeneration(g_gateway)
                      << " active=" << permanentGatewayActive(g_gateway) << '\n';
        }
    }

    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }

    assert(!permanentGatewayAdmissionOpen(g_gateway));
    assert(permanentGatewayHandler(g_gateway) == nullptr);
    assert(permanentGatewayActive(g_gateway) == 0);
    assert(g_handler_calls.load(std::memory_order_relaxed) != 0);
    assert(g_worker_calls.load(std::memory_order_relaxed) != 0);
    assert(rediscoveries == kCycles + 1);

    std::cerr << "stage=permanent-gateway pass cycles=" << kCycles
              << " rediscoveries=" << rediscoveries
              << " worker_calls=" << g_worker_calls.load(std::memory_order_relaxed)
              << " handler_calls=" << g_handler_calls.load(std::memory_order_relaxed)
              << " permanent_rx_bytes=" << g_gateway.permanent_rx_bytes
              << " permanent_rw_bytes=" << g_gateway.permanent_rw_bytes << '\n';
    return 0;
}
