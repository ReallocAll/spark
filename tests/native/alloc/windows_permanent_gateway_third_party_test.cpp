#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_third_party_test.cpp is Windows-only"
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

using spark::stable_entry_experiment::bindPermanentGateway;
using spark::stable_entry_experiment::detachPermanentGateway;
using spark::stable_entry_experiment::discoverPermanentGateway;
using spark::stable_entry_experiment::installPermanentGateway;
using spark::stable_entry_experiment::permanentGatewayActive;
using spark::stable_entry_experiment::permanentGatewayAdmissionOpen;
using spark::stable_entry_experiment::PermanentGatewayHandle;
using spark::stable_entry_experiment::permanentGatewayHandler;
using spark::stable_entry_experiment::permanentGatewayOriginal;

namespace {

using TargetFn = int(__cdecl *)(int);

constexpr std::size_t kWorkers = 6;
constexpr std::size_t kCallsPerWorker = 100000;
constexpr std::uint64_t kTimeoutMs = 5000;

PermanentGatewayHandle g_gateway;
std::atomic<std::uint64_t> g_handler_calls{0};

extern "C" __declspec(noinline) int __cdecl thirdPartyProbeHandler(int value) noexcept
{
    auto *original = reinterpret_cast<TargetFn>(permanentGatewayOriginal(g_gateway));
    if (original == nullptr) {
        std::abort();
    }
    g_handler_calls.fetch_add(1, std::memory_order_relaxed);
    return original(value) + 1000;
}

class ExecutableFunction {
public:
    explicit ExecutableFunction(std::array<std::uint8_t, 16> code)
    {
        page_ =
            static_cast<std::uint8_t *>(::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        assert(page_ != nullptr);
        assert((reinterpret_cast<std::uintptr_t>(page_) & 7U) == 0);
        std::memcpy(page_, code.data(), code.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, code.size()) != FALSE);
        DWORD old_protection = 0;
        assert(::VirtualProtect(page_, 64 * 1024, PAGE_EXECUTE_READ, &old_protection) != FALSE);
    }

    ~ExecutableFunction()
    {
        if (page_ != nullptr) {
            (void)::VirtualFree(page_, 0, MEM_RELEASE);
        }
    }

    template <typename Function>
    [[nodiscard]] Function function() const noexcept
    {
        return reinterpret_cast<Function>(page_);
    }

    [[nodiscard]] void *address() const noexcept { return page_; }

private:
    std::uint8_t *page_ = nullptr;
};

void replacePublicEntryWithThirdParty(void *entry)
{
    // Simulate a third party taking ownership while there are deliberately no
    // callers executing the public entry. This test is about Spark's behavior
    // after ownership is lost, not about blessing unsynchronized live code
    // modification (which the retired-entry experiments already rejected).
    const std::array<std::uint8_t, 8> replacement = {
        0x8D, 0x41, 0x02,  // lea eax,[rcx+2]
        0x66, 0x90,        // nop
        0xC3,              // ret
        0x90, 0x90,
    };

    DWORD old_protection = 0;
    if (::VirtualProtect(entry, replacement.size(), PAGE_EXECUTE_READWRITE, &old_protection) == FALSE) {
        std::cerr << "third-party ownership VirtualProtect writable failed error=" << ::GetLastError() << '\n';
        std::abort();
    }
    std::memcpy(entry, replacement.data(), replacement.size());
    if (::FlushInstructionCache(::GetCurrentProcess(), entry, replacement.size()) == FALSE) {
        std::cerr << "third-party ownership FlushInstructionCache failed error=" << ::GetLastError() << '\n';
        std::abort();
    }
    DWORD ignored = 0;
    if (::VirtualProtect(entry, replacement.size(), old_protection, &ignored) == FALSE) {
        std::cerr << "third-party ownership protection restore failed error=" << ::GetLastError() << '\n';
        std::abort();
    }
}

void requireDiscoveryFailure(void *entry, const char *stage)
{
    PermanentGatewayHandle discovered;
    std::string error;
    if (discoverPermanentGateway(entry, discovered, error)) {
        std::cerr << "third-party ownership discovery unexpectedly succeeded stage=" << stage
                  << " gateway=" << discovered.gateway << " state=" << discovered.state << '\n';
        std::abort();
    }
    if (error.empty()) {
        std::cerr << "third-party ownership discovery failed without fail-closed diagnostic stage=" << stage << '\n';
        std::abort();
    }
    std::cerr << "stage=permanent-gateway-third-party discovery-fail-closed point=" << stage << " error=" << error
              << '\n';
}

}  // namespace

int main()
{
    std::cerr << "stage=permanent-gateway-third-party begin\n";

    ExecutableFunction target({
        0x8D,
        0x41,
        0x01,
        0x66,
        0x90,
        0xC3,
        0x90,
        0x90,
        0x90,
        0x90,
        0x90,
        0x90,
        0x90,
        0x90,
        0x90,
        0x90,
    });
    TargetFn public_entry = target.function<TargetFn>();
    assert(public_entry(40) == 41);

    std::string error;
    if (!installPermanentGateway(target.address(), 0, g_gateway, error)) {
        std::cerr << "third-party ownership install failed: " << error << '\n';
        return 2;
    }
    auto cached_gateway = reinterpret_cast<TargetFn>(g_gateway.gateway);
    if (cached_gateway == nullptr) {
        return 3;
    }

    if (!bindPermanentGateway(g_gateway, reinterpret_cast<void *>(&thirdPartyProbeHandler), kTimeoutMs, error)) {
        std::cerr << "third-party ownership bind failed: " << error << '\n';
        return 4;
    }
    assert(public_entry(7) == 1008);
    assert(g_handler_calls.load(std::memory_order_relaxed) != 0);

    // No public-entry or cached-gateway callers are running while the simulated
    // third party replaces the entry. Spark must not attempt to restore the
    // entry in response: ownership is gone and the permanent gateway stays put.
    replacePublicEntryWithThirdParty(target.address());
    assert(public_entry(7) == 9);
    requireDiscoveryFailure(target.address(), "ownership-lost-before-detach");

    const std::uint64_t calls_before_detach = g_handler_calls.load(std::memory_order_relaxed);
    if (!detachPermanentGateway(g_gateway, kTimeoutMs, error)) {
        std::cerr << "third-party ownership detach failed: " << error << '\n';
        return 5;
    }
    assert(!permanentGatewayAdmissionOpen(g_gateway));
    assert(permanentGatewayActive(g_gateway) == 0);
    assert(permanentGatewayHandler(g_gateway) == nullptr);
    const std::uint64_t calls_after_detach = g_handler_calls.load(std::memory_order_relaxed);
    assert(calls_after_detach == calls_before_detach);

    // A third-party trampoline may have cached/chained the old gateway address.
    // After detach it must remain a process-lifetime pass-through path and must
    // never re-enter an unloadable Spark handler.
    std::atomic<std::uint64_t> pass_through_calls{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = static_cast<int>(worker + 1);
            for (std::size_t iteration = 0; iteration < kCallsPerWorker; ++iteration) {
                const int result = cached_gateway(value);
                if (result != value + 1) {
                    std::cerr << "third-party cached gateway mismatch worker=" << worker << " iteration=" << iteration
                              << " value=" << value << " result=" << result
                              << " active=" << permanentGatewayActive(g_gateway)
                              << " handler=" << permanentGatewayHandler(g_gateway) << '\n';
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                pass_through_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }

    assert(pass_through_calls.load(std::memory_order_relaxed) == kWorkers * kCallsPerWorker);
    assert(g_handler_calls.load(std::memory_order_relaxed) == calls_after_detach);
    assert(permanentGatewayActive(g_gateway) == 0);
    assert(permanentGatewayHandler(g_gateway) == nullptr);
    assert(public_entry(123) == 125);

    // A later Spark image cannot rediscover the gateway from an entry now owned
    // by someone else. That state must be treated as unsupported/ownership-lost;
    // production acquisition must not respond by installing a second island.
    requireDiscoveryFailure(target.address(), "reload-after-ownership-loss");

    std::cerr << "stage=permanent-gateway-third-party pass"
              << " cached_gateway_calls=" << pass_through_calls.load(std::memory_order_relaxed)
              << " handler_calls=" << calls_after_detach << " active=" << permanentGatewayActive(g_gateway) << '\n';
    return 0;
}
