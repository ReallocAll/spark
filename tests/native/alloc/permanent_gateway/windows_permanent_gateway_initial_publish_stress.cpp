#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_initial_publish_stress.cpp is Windows-only"
#endif

#include <windows.h>

#include <array>
#include <atomic>
#include <cassert>
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

constexpr std::size_t kCycles = 1000;
constexpr std::size_t kWorkers = 8;
constexpr std::size_t kTargetReservation = 64 * 1024;

[[noreturn]] void fail(const char *message) noexcept
{
    std::fprintf(stderr, "permanent-gateway-initial-publish FAIL: %s\n", message);
    std::fflush(stderr);
    std::abort();
}

class ProcessLifetimeTarget {
public:
    ProcessLifetimeTarget()
    {
        memory_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, kTargetReservation, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (memory_ == nullptr) {
            fail("VirtualAlloc synthetic target failed");
        }
        // Deliberately begins with a three-byte instruction. The old atomic8
        // publication raced exactly at +3 when bytes 3..4 changed under a live
        // caller; this target keeps that failure mode continuously exercisable.
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

    ~ProcessLifetimeTarget() = default;  // Intentionally process-lifetime after publication.

    [[nodiscard]] void *address() const noexcept { return memory_; }
    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(memory_); }

private:
    std::uint8_t *memory_ = nullptr;
};

}  // namespace

int main()
{
    std::fprintf(stderr, "permanent-gateway-initial-publish begin cycles=%llu workers=%llu\n",
                 static_cast<unsigned long long>(kCycles), static_cast<unsigned long long>(kWorkers));

    std::uint64_t total_calls = 0;
    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        auto *target = new ProcessLifetimeTarget();
        SyntheticFn function = target->function();
        if (function(41) != 42) {
            fail("baseline synthetic semantics failed");
        }

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> calls{0};
        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (std::size_t worker = 0; worker < kWorkers; ++worker) {
            workers.emplace_back([&, worker] {
                int value = static_cast<int>(worker + 1);
                while (!stop.load(std::memory_order_acquire)) {
                    const int result = function(value);
                    if (result != value + 1) {
                        std::fprintf(stderr,
                                     "semantic mismatch cycle=%llu worker=%llu value=%d result=%d calls=%llu\n",
                                     static_cast<unsigned long long>(cycle), static_cast<unsigned long long>(worker),
                                     value, result, static_cast<unsigned long long>(calls.load(std::memory_order_relaxed)));
                        std::abort();
                    }
                    value = (value + 1) & 0x7fff;
                    calls.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Continuously create short-lived callers while the one-time publication
        // runs. A safe fixed-point suspension must either include a just-born
        // thread or suspend its creator before executable bytes are changed.
        std::thread churn([&] {
            while (!stop.load(std::memory_order_acquire)) {
                std::thread transient([&] {
                    for (int value = 0; value < 16; ++value) {
                        if (function(value) != value + 1) {
                            std::abort();
                        }
                        calls.fetch_add(1, std::memory_order_relaxed);
                    }
                });
                transient.join();
                ::SwitchToThread();
            }
        });

        const std::uint64_t ready_deadline = ::GetTickCount64() + 2000;
        while (calls.load(std::memory_order_acquire) < 1000) {
            if (::GetTickCount64() >= ready_deadline) {
                fail("workers did not establish live entry traffic");
            }
            ::SwitchToThread();
        }

        PermanentGateway gateway;
        bool created = false;
        std::string error;
        if (!PermanentGateway::installOrRediscover(target->address(), 0, gateway, created, error)) {
            std::fprintf(stderr, "install failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                         error.c_str());
            fail("guarded permanent publication failed");
        }
        if (!created || !gateway.valid() || !gateway.drained() || gateway.handlerAddress() != nullptr) {
            fail("new permanent gateway did not start closed and drained");
        }

        for (int value = 0; value < 64; ++value) {
            if (function(value) != value + 1) {
                fail("post-publication pass-through semantics failed");
            }
        }

        stop.store(true, std::memory_order_release);
        churn.join();
        for (auto &worker : workers) {
            worker.join();
        }
        total_calls += calls.load(std::memory_order_relaxed);

        // target, gateway island/state and original trampoline intentionally stay
        // resident. Reclaiming any of them would invalidate the safety property
        // this experiment is proving.
        (void)target;

        if ((cycle + 1) % 25 == 0) {
            std::fprintf(stderr, "permanent-gateway-initial-publish progress=%llu/%llu total_calls=%llu\n",
                         static_cast<unsigned long long>(cycle + 1), static_cast<unsigned long long>(kCycles),
                         static_cast<unsigned long long>(total_calls));
            std::fflush(stderr);
        }
    }

    std::fprintf(stderr, "permanent-gateway-initial-publish PASS cycles=%llu total_calls=%llu\n",
                 static_cast<unsigned long long>(kCycles), static_cast<unsigned long long>(total_calls));
    return 0;
}
