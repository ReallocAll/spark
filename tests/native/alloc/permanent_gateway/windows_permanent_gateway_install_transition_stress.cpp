#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_install_transition_stress.cpp is Windows-only"
#endif

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using spark::permanent_gateway_experiment::PermanentGateway;

namespace {
using SyntheticFn = int(__cdecl *)(int);

constexpr std::size_t kTargetReservation = 64 * 1024;
constexpr std::size_t kCycles = 1000;
constexpr std::size_t kWorkers = 8;
constexpr std::uint64_t kWarmCallsPerCycle = 4096;

std::atomic<std::uint64_t> g_cycle{0};
std::atomic<void *> g_target{nullptr};
std::atomic<SyntheticFn> g_current{nullptr};
std::atomic<std::uint64_t> g_worker_calls{0};
std::atomic<bool> g_stop{false};

[[noreturn]] void fail(const char *message) noexcept
{
    std::fprintf(stderr, "permanent-gateway-install-transition FAIL: %s cycle=%llu target=%p calls=%llu\n", message,
                 static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
                 g_target.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_worker_calls.load(std::memory_order_relaxed)));
    std::fflush(stderr);
    std::abort();
}

LONG WINAPI exceptionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    const DWORD code = exception != nullptr && exception->ExceptionRecord != nullptr
                           ? exception->ExceptionRecord->ExceptionCode
                           : 0;
    const void *fault = exception != nullptr && exception->ExceptionRecord != nullptr &&
                                exception->ExceptionRecord->NumberParameters >= 2
                            ? reinterpret_cast<void *>(exception->ExceptionRecord->ExceptionInformation[1])
                            : nullptr;
    std::uintptr_t rip = 0;
#if defined(_M_X64)
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        rip = static_cast<std::uintptr_t>(exception->ContextRecord->Rip);
    }
#endif
    std::fprintf(stderr,
                 "permanent-gateway-install-transition exception code=0x%08lx rip=0x%llx fault=%p cycle=%llu "
                 "target=%p calls=%llu\n",
                 static_cast<unsigned long>(code), static_cast<unsigned long long>(rip), fault,
                 static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
                 g_target.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_worker_calls.load(std::memory_order_relaxed)));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

class Target {
public:
    Target()
    {
        memory_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, kTargetReservation, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (memory_ == nullptr) {
            fail("VirtualAlloc target failed");
        }
        constexpr std::array<std::uint8_t, 16> bytes{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(memory_, bytes.data(), bytes.size());
        if (::FlushInstructionCache(::GetCurrentProcess(), memory_, bytes.size()) == FALSE) {
            fail("FlushInstructionCache target failed");
        }
        DWORD old = 0;
        if (::VirtualProtect(memory_, kTargetReservation, PAGE_EXECUTE_READ, &old) == FALSE) {
            fail("VirtualProtect target RX failed");
        }
    }

    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(memory_); }
    [[nodiscard]] void *address() const noexcept { return memory_; }

private:
    std::uint8_t *memory_ = nullptr;
};

void worker(std::size_t worker_index)
{
    int value = static_cast<int>(worker_index + 1);
    while (!g_stop.load(std::memory_order_acquire)) {
        SyntheticFn function = g_current.load(std::memory_order_acquire);
        if (function == nullptr) {
            ::SwitchToThread();
            continue;
        }
        const int result = function(value);
        if (result != value + 1) {
            std::fprintf(stderr,
                         "permanent-gateway-install-transition mismatch worker=%llu value=%d result=%d cycle=%llu "
                         "target=%p\n",
                         static_cast<unsigned long long>(worker_index), value, result,
                         static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
                         g_target.load(std::memory_order_relaxed));
            std::fflush(stderr);
            std::abort();
        }
        value = (value + 1) & 0x7fff;
        g_worker_calls.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

int main()
{
    std::fprintf(stderr, "permanent-gateway-install-transition begin cycles=%llu workers=%llu warm_calls=%llu\n",
                 static_cast<unsigned long long>(kCycles), static_cast<unsigned long long>(kWorkers),
                 static_cast<unsigned long long>(kWarmCallsPerCycle));
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = ::SetUnhandledExceptionFilter(&exceptionFilter);

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t index = 0; index < kWorkers; ++index) {
        workers.emplace_back(&worker, index);
    }

    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        g_cycle.store(cycle, std::memory_order_release);
        // Deliberately process-lifetime. Once patched, the target and gateway may
        // still be executed by a worker that loaded the previous function pointer.
        auto *target = new Target();
        SyntheticFn function = target->function();
        if (function(41) != 42) {
            fail("baseline semantics failed");
        }
        g_target.store(target->address(), std::memory_order_release);
        const std::uint64_t before = g_worker_calls.load(std::memory_order_acquire);
        g_current.store(function, std::memory_order_release);
        const std::uint64_t deadline = ::GetTickCount64() + 5000;
        while (g_worker_calls.load(std::memory_order_acquire) - before < kWarmCallsPerCycle) {
            if (::GetTickCount64() >= deadline) {
                fail("workers did not warm target before publication");
            }
            ::SwitchToThread();
        }

        PermanentGateway gateway;
        bool created = false;
        std::string error;
        if (!PermanentGateway::installOrRediscover(target->address(), 0, gateway, created, error)) {
            std::fprintf(stderr, "install failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                         error.c_str());
            return 2;
        }
        if (!created || !gateway.valid() || !gateway.drained() || gateway.handlerAddress() != nullptr) {
            fail("published gateway state invalid");
        }
        for (int value = 0; value < 64; ++value) {
            if (function(value) != value + 1) {
                fail("post-publication pass-through semantics failed");
            }
        }

        if ((cycle + 1) % 25 == 0) {
            const auto footprint = gateway.footprint();
            std::fprintf(stderr,
                         "permanent-gateway-install-transition progress=%llu/%llu calls=%llu footprint=%llu\n",
                         static_cast<unsigned long long>(cycle + 1), static_cast<unsigned long long>(kCycles),
                         static_cast<unsigned long long>(g_worker_calls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(footprint.island_committed + footprint.state_committed +
                                                         footprint.trampoline_committed));
        }
    }

    g_stop.store(true, std::memory_order_release);
    for (std::thread &thread : workers) {
        thread.join();
    }
    (void)::SetUnhandledExceptionFilter(previous);
    std::fprintf(stderr, "permanent-gateway-install-transition PASS cycles=%llu calls=%llu\n",
                 static_cast<unsigned long long>(kCycles),
                 static_cast<unsigned long long>(g_worker_calls.load(std::memory_order_relaxed)));
    return 0;
}
