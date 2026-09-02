#include "native/alloc/windows_stable_entry_permanent_gateway.h"

#ifndef _WIN32
#error "windows_stable_entry_permanent_gateway_test.cpp is Windows-only"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::PermanentGateway;
using spark::stable_entry_experiment::PermanentGatewayArity;

namespace {

using SyntheticFn = int(__cdecl *)(int);

std::atomic<std::uint64_t> g_handler_calls{0};
std::atomic<bool> g_block_handler{false};
std::atomic<std::uint32_t> g_block_entered{0};
std::atomic<std::uint64_t> g_cycle{0};

extern "C" __declspec(noinline) int __cdecl countingHandler(int value) noexcept
{
    g_handler_calls.fetch_add(1, std::memory_order_relaxed);
    return value + 1;
}

extern "C" __declspec(noinline) int __cdecl blockingHandler(int value) noexcept
{
    g_handler_calls.fetch_add(1, std::memory_order_relaxed);
    g_block_entered.fetch_add(1, std::memory_order_release);
    while (g_block_handler.load(std::memory_order_acquire)) {
        ::SwitchToThread();
    }
    return value + 1;
}

class ExecutableSyntheticFunction {
public:
    ExecutableSyntheticFunction()
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(page_ != nullptr);
        assert((reinterpret_cast<std::uintptr_t>(page_) & 7U) == 0);
        // lea eax,[rcx+1]; 2-byte nop; ret; padding. The bounded relocator
        // therefore owns five bytes, inside the gateway's atomic eight-byte CAS.
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(page_, code.data(), code.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, code.size()) != FALSE);
        DWORD old = 0;
        assert(::VirtualProtect(page_, 64 * 1024, PAGE_EXECUTE_READ, &old) != FALSE);
    }

    ~ExecutableSyntheticFunction()
    {
        if (page_ != nullptr) {
            // The test target itself is not process-lifetime production state;
            // workers are joined before teardown.
            ::VirtualFree(page_, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(page_); }
    [[nodiscard]] void *address() const noexcept { return page_; }

private:
    std::uint8_t *page_ = nullptr;
};

[[noreturn]] void fail(const char *stage, const PermanentGateway &gateway, const std::string &error)
{
    std::fprintf(stderr,
                 "stage=permanent-gateway failure=%s cycle=%llu generation=%u active=%u closed=%d handler=%p "
                 "entry=%p gateway=%p state=%p trampoline=%p error=%s\n",
                 stage, static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
                 gateway.generation(), gateway.activeCount(), gateway.admissionClosed() ? 1 : 0, gateway.handler(),
                 gateway.entry(), gateway.gateway(), gateway.state(), gateway.originalTrampoline(), error.c_str());
    std::fflush(stderr);
    std::abort();
}

void proveDetachWaitsForAdmittedCallback(PermanentGateway &gateway, SyntheticFn function)
{
    std::string error;
    g_block_entered.store(0, std::memory_order_release);
    g_block_handler.store(true, std::memory_order_release);
    if (!gateway.attach(reinterpret_cast<void *>(&blockingHandler), error)) {
        fail("blocking-attach", gateway, error);
    }

    std::atomic<bool> caller_done{false};
    std::thread caller([&] {
        const int result = function(10);
        assert(result == 11);
        caller_done.store(true, std::memory_order_release);
    });
    const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_block_entered.load(std::memory_order_acquire) == 0) {
        if (std::chrono::steady_clock::now() >= enter_deadline) {
            fail("blocking-handler-never-entered", gateway, "handler admission did not occur");
        }
        std::this_thread::yield();
    }
    assert(gateway.activeCount() != 0);

    std::atomic<bool> detach_done{false};
    std::atomic<bool> detach_ok{false};
    std::string detach_error;
    std::thread detacher([&] {
        detach_ok.store(gateway.detach(5000, detach_error), std::memory_order_release);
        detach_done.store(true, std::memory_order_release);
    });

    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gateway.admissionClosed()) {
        if (std::chrono::steady_clock::now() >= close_deadline) {
            fail("blocking-gate-never-closed", gateway, "detach failed to close admission");
        }
        std::this_thread::yield();
    }
    // The admitted callback is still executing. Detach must not clear the
    // handler or complete while its active token remains outstanding.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (detach_done.load(std::memory_order_acquire) || gateway.handler() == nullptr || gateway.activeCount() == 0) {
        fail("blocking-detach-returned-early", gateway, "handler lifetime escaped active-token drain");
    }

    // New calls after closure must bypass the blocked handler and use the
    // permanent original trampoline immediately.
    assert(function(20) == 21);
    assert(g_block_entered.load(std::memory_order_acquire) == 1);

    g_block_handler.store(false, std::memory_order_release);
    caller.join();
    detacher.join();
    if (!detach_ok.load(std::memory_order_acquire)) {
        fail("blocking-detach", gateway, detach_error);
    }
    assert(caller_done.load(std::memory_order_acquire));
    assert(gateway.drained());
    assert(gateway.handler() == nullptr);
}

}  // namespace

int main()
{
    std::fprintf(stderr, "stage=permanent-gateway begin\n");
    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    assert(function(41) == 42);

    PermanentGateway gateway;
    std::string error;
    if (!PermanentGateway::installOrRediscover(target.address(), PermanentGatewayArity::UpToFourIntegerArgs, gateway,
                                               error)) {
        fail("install", gateway, error);
    }
    const void *stable_gateway = gateway.gateway();
    const void *stable_state = gateway.state();
    const void *stable_trampoline = gateway.originalTrampoline();
    const auto footprint = gateway.footprint();
    std::fprintf(stderr,
                 "stage=permanent-gateway installed entry=%p gateway=%p state=%p trampoline=%p "
                 "island_reserved=%llu island_committed=%llu trampoline_allocation=%llu\n",
                 gateway.entry(), gateway.gateway(), gateway.state(), gateway.originalTrampoline(),
                 static_cast<unsigned long long>(footprint.island_reserved),
                 static_cast<unsigned long long>(footprint.island_committed),
                 static_cast<unsigned long long>(footprint.trampoline_reserved_committed));
    assert(gateway.admissionClosed());
    assert(gateway.drained());
    assert(gateway.handler() == nullptr);
    assert(function(41) == 42);

    proveDetachWaitsForAdmittedCallback(gateway, function);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> worker_calls{0};
    constexpr std::size_t kWorkers = 8;
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = static_cast<int>(worker + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const int result = function(value);
                if (result != value + 1) {
                    std::fprintf(stderr,
                                 "stage=permanent-gateway mismatch worker=%llu cycle=%llu value=%d result=%d "
                                 "generation=%u active=%u closed=%d handler=%p\n",
                                 static_cast<unsigned long long>(worker),
                                 static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)), value,
                                 result, gateway.generation(), gateway.activeCount(), gateway.admissionClosed() ? 1 : 0,
                                 gateway.handler());
                    std::fflush(stderr);
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    constexpr std::size_t kCycles = 1200;
    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        g_cycle.store(cycle + 1, std::memory_order_release);
        if (!gateway.attach(reinterpret_cast<void *>(&countingHandler), error)) {
            fail("stress-attach", gateway, error);
        }

        const std::uint64_t before = g_handler_calls.load(std::memory_order_relaxed);
        const auto call_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_handler_calls.load(std::memory_order_relaxed) == before) {
            if (std::chrono::steady_clock::now() >= call_deadline) {
                fail("stress-handler-not-called", gateway, "workers did not enter attached handler");
            }
            std::this_thread::yield();
        }

        std::thread transient;
        if ((cycle % 10) == 0) {
            transient = std::thread([&] {
                for (int i = 0; i < 1000; ++i) {
                    assert(function(i) == i + 1);
                }
            });
        }

        if (!gateway.detach(5000, error)) {
            fail("stress-detach", gateway, error);
        }
        if (transient.joinable()) {
            transient.join();
        }
        assert(gateway.admissionClosed());
        assert(gateway.drained());
        assert(gateway.handler() == nullptr);

        // Simulate a freshly loaded Spark generation which has no C++ object
        // from the previous generation: rediscover exclusively from entry bytes.
        PermanentGateway rediscovered;
        if (!PermanentGateway::installOrRediscover(target.address(), PermanentGatewayArity::UpToFourIntegerArgs,
                                                   rediscovered, error)) {
            fail("rediscover", gateway, error);
        }
        if (rediscovered.gateway() != stable_gateway || rediscovered.state() != stable_state ||
            rediscovered.originalTrampoline() != stable_trampoline) {
            fail("rediscover-created-new-island", rediscovered, "permanent addresses changed across generation");
        }
        gateway = rediscovered;

        // Closed mode must remain safe and semantically identical to original.
        for (int i = 0; i < 32; ++i) {
            assert(function(i) == i + 1);
        }
        if ((cycle + 1) % 100 == 0) {
            std::fprintf(stderr,
                         "stage=permanent-gateway progress=%llu/%llu generation=%u calls=%llu handler_calls=%llu "
                         "active=%u\n",
                         static_cast<unsigned long long>(cycle + 1), static_cast<unsigned long long>(kCycles),
                         gateway.generation(),
                         static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_handler_calls.load(std::memory_order_relaxed)),
                         gateway.activeCount());
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }
    assert(gateway.admissionClosed());
    assert(gateway.drained());
    assert(gateway.handler() == nullptr);
    assert(worker_calls.load(std::memory_order_relaxed) != 0);
    assert(g_handler_calls.load(std::memory_order_relaxed) != 0);
    std::fprintf(stderr,
                 "stage=permanent-gateway pass cycles=%llu generation=%u calls=%llu handler_calls=%llu "
                 "gateway=%p state=%p trampoline=%p\n",
                 static_cast<unsigned long long>(kCycles), gateway.generation(),
                 static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_handler_calls.load(std::memory_order_relaxed)), gateway.gateway(),
                 gateway.state(), gateway.originalTrampoline());
    return 0;
}
