#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_hot_reload_runtime_test.cpp is Windows-only"
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
using ConfigureFn = BOOL __cdecl(void *, volatile LONG64 *, volatile LONG64 *, volatile LONG64 *, volatile LONG *,
                                 void *, std::size_t);
using HandlerFn = int __cdecl(int);

constexpr std::size_t kTargetReservation = 64 * 1024;
constexpr std::size_t kCycles = 1000;
constexpr std::size_t kWorkers = 6;

[[noreturn]] void fail(const char *message) noexcept
{
    std::fprintf(stderr, "permanent-gateway-hot-reload-runtime FAIL: %s\n", message);
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] LONG64 load64(volatile LONG64 *value) noexcept
{
    return ::InterlockedCompareExchange64(value, 0, 0);
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

    [[nodiscard]] void *address() const noexcept { return memory_; }
    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(memory_); }

private:
    std::uint8_t *memory_ = nullptr;
};

struct ModuleRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

[[nodiscard]] ModuleRange moduleRange(HMODULE module) noexcept
{
    ModuleRange result;
    if (module == nullptr) {
        return result;
    }
    const auto *base = reinterpret_cast<const std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return result;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return result;
    }
    result.begin = reinterpret_cast<std::uintptr_t>(base);
    result.end = result.begin + nt->OptionalHeader.SizeOfImage;
    return result;
}

[[nodiscard]] bool contains(ModuleRange range, std::uintptr_t value) noexcept
{
    return range.begin != 0 && range.begin <= value && value < range.end;
}

[[nodiscard]] bool permanentStorageContainsModulePointer(const PermanentGateway &gateway, ModuleRange range) noexcept
{
    const auto footprint = gateway.footprint();
    struct Region {
        const void *base;
        std::size_t size;
    };
    const Region regions[] = {{gateway.islandBase(), footprint.island_committed},
                              {gateway.stateBase(), footprint.state_committed}};
    for (const Region &region : regions) {
        if (region.base == nullptr || region.size < sizeof(std::uintptr_t)) {
            continue;
        }
        const auto *bytes = static_cast<const std::uint8_t *>(region.base);
        for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= region.size; offset += sizeof(std::uintptr_t)) {
            std::uintptr_t value = 0;
            std::memcpy(&value, bytes + offset, sizeof(value));
            if (contains(range, value)) {
                std::fprintf(stderr, "stale module pointer region=%p offset=%llu value=0x%llx\n", region.base,
                             static_cast<unsigned long long>(offset), static_cast<unsigned long long>(value));
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool waitForActive(const PermanentGateway &gateway, std::uint64_t timeout_ms) noexcept
{
    const std::uint64_t deadline = ::GetTickCount64() + timeout_ms;
    while (gateway.activeCount() == 0) {
        if (::GetTickCount64() >= deadline) {
            return false;
        }
        ::SwitchToThread();
    }
    return true;
}

}  // namespace

int main()
{
    std::fprintf(stderr, "permanent-gateway-hot-reload-runtime begin cycles=%llu workers=%llu\n",
                 static_cast<unsigned long long>(kCycles), static_cast<unsigned long long>(kWorkers));

    Target target;
    SyntheticFn function = target.function();
    if (function(41) != 42) {
        fail("baseline semantics failed");
    }

    PermanentGateway gateway;
    bool created = false;
    std::string error;
    if (!PermanentGateway::installOrRediscover(target.address(), 0, gateway, created, error)) {
        std::fprintf(stderr, "initial gateway install failed: %s\n", error.c_str());
        return 2;
    }
    if (!created || !gateway.valid() || !gateway.drained() || gateway.handlerAddress() != nullptr || function(9) != 10) {
        fail("initial closed gateway invalid");
    }

    const void *const island = gateway.islandBase();
    const void *const state = gateway.stateBase();
    void *const trampoline = gateway.originalTrampoline();
    const void *const gateway_entry = gateway.gatewayEntry();
    const std::size_t gateway_code_size = gateway.gatewayCodeSize();
    const auto footprint = gateway.footprint();
    std::fprintf(stderr, "permanent-gateway footprint island=%llu state=%llu trampoline=%llu total=%llu\n",
                 static_cast<unsigned long long>(footprint.island_committed),
                 static_cast<unsigned long long>(footprint.state_committed),
                 static_cast<unsigned long long>(footprint.trampoline_committed),
                 static_cast<unsigned long long>(footprint.island_committed + footprint.state_committed +
                                                 footprint.trampoline_committed));

    volatile LONG64 handler_calls = 0;
    volatile LONG64 handler_entered = 0;
    volatile LONG64 stack_ok = 0;
    volatile LONG hold = 0;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> worker_calls{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker_index = 0; worker_index < kWorkers; ++worker_index) {
        workers.emplace_back([&, worker_index] {
            int value = static_cast<int>(worker_index + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const int result = function(value);
                if (result != value + 1) {
                    std::fprintf(stderr, "worker mismatch worker=%llu value=%d result=%d generation=%u active=%u\n",
                                 static_cast<unsigned long long>(worker_index), value, result, gateway.generation(),
                                 gateway.activeCount());
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        HMODULE module = ::LoadLibraryW(L"windows_permanent_gateway_test_handler.dll");
        if (module == nullptr) {
            std::fprintf(stderr, "LoadLibrary failed cycle=%llu error=%lu\n", static_cast<unsigned long long>(cycle),
                         static_cast<unsigned long>(::GetLastError()));
            fail("handler DLL load failed");
        }
        const ModuleRange module_range = moduleRange(module);
        if (module_range.begin == 0 || module_range.end <= module_range.begin) {
            fail("handler module range invalid");
        }
        auto *configure = reinterpret_cast<ConfigureFn *>(::GetProcAddress(module, "configure_gateway_handler"));
        auto *handler = reinterpret_cast<HandlerFn *>(::GetProcAddress(module, "gateway_test_handler"));
        if (configure == nullptr || handler == nullptr) {
            fail("handler exports missing");
        }

        PermanentGateway rediscovered;
        bool rediscovered_created = false;
        if (!PermanentGateway::installOrRediscover(target.address(), 0, rediscovered, rediscovered_created, error)) {
            std::fprintf(stderr, "rediscovery failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                         error.c_str());
            fail("rediscovery failed");
        }
        if (rediscovered_created || rediscovered.islandBase() != island || rediscovered.stateBase() != state ||
            rediscovered.originalTrampoline() != trampoline || rediscovered.gatewayEntry() != gateway_entry ||
            rediscovered.gatewayCodeSize() != gateway_code_size) {
            fail("gateway rediscovery did not reuse exact permanent state");
        }
        if (!configure(trampoline, &handler_calls, &handler_entered, &stack_ok, &hold,
                       const_cast<void *>(gateway_entry), gateway_code_size)) {
            fail("handler configuration failed");
        }

        const std::uint64_t cookie = static_cast<std::uint64_t>(cycle) + 1;
        const bool force_drain = (cycle % 100) == 99;
        (void)::InterlockedExchange(&hold, force_drain ? 1 : 0);
        if (!rediscovered.attach(reinterpret_cast<void *>(handler), cookie, error)) {
            std::fprintf(stderr, "attach failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                         error.c_str());
            fail("attach failed");
        }

        if (force_drain) {
            if (!waitForActive(rediscovered, 2000)) {
                fail("could not hold an admitted callback");
            }
            std::atomic<bool> done{false};
            std::atomic<bool> ok{false};
            std::string detach_error;
            std::thread detacher([&] {
                ok.store(rediscovered.detach(cookie, 5000, detach_error), std::memory_order_release);
                done.store(true, std::memory_order_release);
            });
            ::Sleep(10);
            if (done.load(std::memory_order_acquire) || rediscovered.activeCount() == 0) {
                fail("detach did not wait for admitted callback");
            }
            (void)::InterlockedExchange(&hold, 0);
            detacher.join();
            if (!ok.load(std::memory_order_acquire)) {
                std::fprintf(stderr, "held detach failed cycle=%llu error=%s\n",
                             static_cast<unsigned long long>(cycle), detach_error.c_str());
                fail("held detach failed");
            }
        }
        else {
            for (int value = 0; value < 64; ++value) {
                if (function(value) != value + 1) {
                    fail("attached semantics failed");
                }
            }
            if (!rediscovered.detach(cookie, 5000, error)) {
                std::fprintf(stderr, "detach failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                             error.c_str());
                fail("detach failed");
            }
        }

        if (!rediscovered.drained() || rediscovered.activeCount() != 0 || rediscovered.handlerAddress() != nullptr) {
            fail("detached gateway retained active callback or handler pointer");
        }
        if (permanentStorageContainsModulePointer(rediscovered, module_range)) {
            fail("permanent storage retained unloadable module pointer");
        }
        const LONG64 calls_before_unload = load64(&handler_calls);
        if (::FreeLibrary(module) == FALSE) {
            fail("FreeLibrary failed");
        }
        if (::GetModuleHandleW(L"windows_permanent_gateway_test_handler.dll") != nullptr) {
            fail("handler DLL remained loaded");
        }
        for (int value = 0; value < 256; ++value) {
            if (function(value) != value + 1) {
                fail("post-unload pass-through failed");
            }
        }
        ::SwitchToThread();
        if (load64(&handler_calls) != calls_before_unload) {
            fail("post-unload call reached stale handler");
        }

        if ((cycle + 1) % 25 == 0) {
            std::fprintf(stderr,
                         "permanent-gateway-hot-reload-runtime progress=%llu/%llu generation=%u active=%u "
                         "worker_calls=%llu handler_calls=%lld stack_ok=%lld\n",
                         static_cast<unsigned long long>(cycle + 1), static_cast<unsigned long long>(kCycles),
                         rediscovered.generation(), rediscovered.activeCount(),
                         static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                         static_cast<long long>(load64(&handler_calls)), static_cast<long long>(load64(&stack_ok)));
        }
    }

    stop.store(true, std::memory_order_release);
    for (std::thread &thread : workers) {
        thread.join();
    }
    if (load64(&stack_ok) != static_cast<LONG64>(kCycles)) {
        std::fprintf(stderr, "stack fidelity mismatch expected=%llu actual=%lld\n",
                     static_cast<unsigned long long>(kCycles), static_cast<long long>(load64(&stack_ok)));
        return 3;
    }
    if (!gateway.drained() || gateway.handlerAddress() != nullptr || function(77) != 78) {
        fail("final gateway state invalid");
    }
    std::fprintf(stderr,
                 "permanent-gateway-hot-reload-runtime PASS cycles=%llu worker_calls=%llu handler_calls=%lld "
                 "entered=%lld stack_ok=%lld generation=%u\n",
                 static_cast<unsigned long long>(kCycles),
                 static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                 static_cast<long long>(load64(&handler_calls)), static_cast<long long>(load64(&handler_entered)),
                 static_cast<long long>(load64(&stack_ok)), gateway.generation());
    return 0;
}
