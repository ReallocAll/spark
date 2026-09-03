#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_publication_test.cpp is Windows-only"
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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::discoverPermanentGateway;
using spark::stable_entry_experiment::installPermanentGateway;
using spark::stable_entry_experiment::PermanentGatewayHandle;

namespace {

using SyntheticFn = int(__cdecl *)(int);

constexpr std::size_t kPublicationRounds = 64;
constexpr std::size_t kWorkers = 8;
constexpr std::size_t kChurners = 2;
constexpr std::uint64_t kPrePublicationCalls = 20000;
constexpr std::uint64_t kPostPublicationCalls = 20000;
constexpr std::uint64_t kChurnBatchCalls = 128;
constexpr std::uint64_t kCounterTimeoutMs = 5000;

[[nodiscard]] bool isExecutable(DWORD protection) noexcept
{
    const DWORD base = protection & 0xFFU;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] void *allocateProcessLifetimeTarget()
{
    constexpr std::array<std::uint8_t, 16> code{
        0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    };

    auto *page = static_cast<std::uint8_t *>(
        ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(page != nullptr);
    assert((reinterpret_cast<std::uintptr_t>(page) & 7U) == 0);
    std::memcpy(page, code.data(), code.size());
    assert(::FlushInstructionCache(::GetCurrentProcess(), page, code.size()) != FALSE);
    DWORD old_protection = 0;
    assert(::VirtualProtect(page, 64 * 1024, PAGE_EXECUTE_READ, &old_protection) != FALSE);

    // Match the production lifetime invariant: once an entry is published, the
    // entry backing allocation remains valid until process exit. The test
    // intentionally leaves these tiny synthetic targets allocated.
    return page;
}

void waitForCounter(const std::atomic<std::uint64_t> &counter, std::uint64_t expected, const char *label,
                    std::size_t round)
{
    const std::uint64_t deadline = ::GetTickCount64() + kCounterTimeoutMs;
    while (counter.load(std::memory_order_acquire) < expected) {
        if (::GetTickCount64() >= deadline) {
            std::cerr << "permanent-gateway publication counter timeout label=" << label << " round=" << round
                      << " current=" << counter.load(std::memory_order_relaxed) << " expected=" << expected << '\n';
            std::abort();
        }
        std::this_thread::yield();
    }
}

void verifyPermanentMemoryPolicy(const PermanentGatewayHandle &handle)
{
    MEMORY_BASIC_INFORMATION code_memory{};
    MEMORY_BASIC_INFORMATION state_memory{};
    assert(::VirtualQuery(handle.gateway, &code_memory, sizeof(code_memory)) != 0);
    assert(::VirtualQuery(handle.state, &state_memory, sizeof(state_memory)) != 0);
    assert(code_memory.State == MEM_COMMIT);
    assert(state_memory.State == MEM_COMMIT);
    assert(isExecutable(code_memory.Protect));
    assert((code_memory.Protect & 0xFFU) != PAGE_EXECUTE_READWRITE);
    assert((code_memory.Protect & 0xFFU) != PAGE_EXECUTE_WRITECOPY);
    assert(!isExecutable(state_memory.Protect));
}

void runPublicationRound(std::size_t round)
{
    void *entry = allocateProcessLifetimeTarget();
    auto function = reinterpret_cast<SyntheticFn>(entry);
    assert(function(41) == 42);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> churn_calls{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = static_cast<int>(worker + 1);
            while (!stop.load(std::memory_order_acquire)) {
                if (function(value) != value + 1) {
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                calls.fetch_add(1, std::memory_order_release);
            }
        });
    }

    std::vector<std::thread> churners;
    churners.reserve(kChurners);
    for (std::size_t churner = 0; churner < kChurners; ++churner) {
        churners.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                std::thread short_lived([&] {
                    for (std::uint64_t call = 0; call < kChurnBatchCalls; ++call) {
                        const int value = static_cast<int>(call & 0x7fffU);
                        if (function(value) != value + 1) {
                            std::abort();
                        }
                        churn_calls.fetch_add(1, std::memory_order_release);
                    }
                });
                short_lived.join();
            }
        });
    }

    waitForCounter(calls, kPrePublicationCalls, "pre-publication-long-lived", round);
    waitForCounter(churn_calls, kChurnBatchCalls, "pre-publication-thread-churn", round);

    PermanentGatewayHandle handle;
    std::string error;
    if (!installPermanentGateway(entry, 0, handle, error)) {
        std::cerr << "permanent-gateway concurrent publication failed round=" << round << " error=" << error << '\n';
        std::abort();
    }
    assert(handle.entry == entry);
    assert(handle.gateway != nullptr);
    assert(handle.state != nullptr);
    assert(handle.original != nullptr);
    verifyPermanentMemoryPolicy(handle);

    const std::uint64_t post_target = calls.load(std::memory_order_acquire) + kPostPublicationCalls;
    waitForCounter(calls, post_target, "post-publication-long-lived", round);
    assert(function(9) == 10);

    PermanentGatewayHandle discovered;
    if (!discoverPermanentGateway(entry, discovered, error)) {
        std::cerr << "permanent-gateway publication rediscovery failed round=" << round << " error=" << error << '\n';
        std::abort();
    }
    assert(discovered.entry == handle.entry);
    assert(discovered.gateway == handle.gateway);
    assert(discovered.state == handle.state);
    assert(discovered.original == handle.original);
    verifyPermanentMemoryPolicy(discovered);

    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }
    for (std::thread &churner : churners) {
        churner.join();
    }

    assert(calls.load(std::memory_order_relaxed) >= kPrePublicationCalls + kPostPublicationCalls);
    assert(churn_calls.load(std::memory_order_relaxed) >= kChurnBatchCalls);
    if ((round + 1) % 8 == 0) {
        std::cerr << "stage=permanent-gateway-publication progress=" << (round + 1) << '/' << kPublicationRounds
                  << " long_lived_calls=" << calls.load(std::memory_order_relaxed)
                  << " churn_calls=" << churn_calls.load(std::memory_order_relaxed) << '\n';
    }
}

}  // namespace

int main()
{
    std::cerr << "stage=permanent-gateway-publication begin rounds=" << kPublicationRounds
              << " workers=" << kWorkers << " churners=" << kChurners << '\n';
    for (std::size_t round = 0; round < kPublicationRounds; ++round) {
        runPublicationRound(round);
    }
    std::cerr << "stage=permanent-gateway-publication pass rounds=" << kPublicationRounds << '\n';
    return 0;
}
