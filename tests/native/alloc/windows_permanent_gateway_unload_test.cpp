#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_unload_test.cpp is Windows-only"
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
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::PermanentGatewayHandle;
using spark::stable_entry_experiment::bindPermanentGateway;
using spark::stable_entry_experiment::detachPermanentGateway;
using spark::stable_entry_experiment::discoverPermanentGateway;
using spark::stable_entry_experiment::installPermanentGateway;
using spark::stable_entry_experiment::permanentGatewayActive;
using spark::stable_entry_experiment::permanentGatewayAdmissionOpen;
using spark::stable_entry_experiment::permanentGatewayGeneration;
using spark::stable_entry_experiment::permanentGatewayHandler;

namespace {

using TargetFn = int(__cdecl *)(int);
using HandlerFn = int(__cdecl *)(int);
using SetHoldFn = void(__cdecl *)(int);
using ResetEnteredFn = void(__cdecl *)();
using EnteredFn = int(__cdecl *)();

constexpr std::size_t kWorkers = 4;
constexpr std::size_t kUnloadCycles = 500;
constexpr std::uint64_t kTimeoutMs = 5000;
constexpr wchar_t kHandlerName[] = L"windows_gateway_test_handler.dll";

class ExecutableFunction {
public:
    explicit ExecutableFunction(std::array<std::uint8_t, 16> code)
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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

[[nodiscard]] std::wstring handlerPath()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        std::cerr << "gateway-unload could not resolve host executable path error=" << ::GetLastError() << '\n';
        std::abort();
    }
    std::wstring path(buffer.data(), length);
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        std::cerr << "gateway-unload host executable path has no directory\n";
        std::abort();
    }
    path.resize(slash + 1);
    path.append(kHandlerName);
    return path;
}

template <typename Function>
[[nodiscard]] Function requiredExport(HMODULE module, const char *name)
{
    FARPROC proc = ::GetProcAddress(module, name);
    if (proc == nullptr) {
        std::cerr << "gateway-unload missing export " << name << " error=" << ::GetLastError() << '\n';
        std::abort();
    }
    return reinterpret_cast<Function>(proc);
}

}  // namespace

int main()
{
    std::cerr << "stage=permanent-gateway-dll-unload begin cycles=" << kUnloadCycles << '\n';

    ExecutableFunction target({
        0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    });
    TargetFn function = target.function<TargetFn>();
    assert(function(41) == 42);

    PermanentGatewayHandle gateway;
    std::string error;
    if (!installPermanentGateway(target.address(), 0, gateway, error)) {
        std::cerr << "gateway-unload install failed: " << error << '\n';
        return 2;
    }
    const void *stable_gateway = gateway.gateway;
    const void *stable_state = gateway.state;
    const void *stable_original = gateway.original;
    const std::wstring dll_path = handlerPath();

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> worker_calls{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = static_cast<int>(worker + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const int result = function(value);
                if (result != value + 1 && result != value + 1000) {
                    std::cerr << "gateway-unload worker mismatch worker=" << worker
                              << " value=" << value << " result=" << result
                              << " active=" << permanentGatewayActive(gateway) << '\n';
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::uint64_t rediscoveries = 0;
    for (std::size_t cycle = 0; cycle < kUnloadCycles; ++cycle) {
        HMODULE module = ::LoadLibraryW(dll_path.c_str());
        if (module == nullptr) {
            std::cerr << "gateway-unload LoadLibrary failed cycle=" << cycle
                      << " error=" << ::GetLastError() << '\n';
            std::abort();
        }

        HandlerFn handler = requiredExport<HandlerFn>(module, "windowsGatewayTestHandler");
        SetHoldFn set_hold = requiredExport<SetHoldFn>(module, "windowsGatewayTestSetHold");
        ResetEnteredFn reset_entered = requiredExport<ResetEnteredFn>(module, "windowsGatewayTestResetEntered");
        EnteredFn entered = requiredExport<EnteredFn>(module, "windowsGatewayTestEntered");

        reset_entered();
        set_hold(1);
        if (!bindPermanentGateway(gateway, reinterpret_cast<void *>(handler), kTimeoutMs, error)) {
            std::cerr << "gateway-unload bind failed cycle=" << cycle << " error=" << error << '\n';
            std::abort();
        }
        assert(permanentGatewayAdmissionOpen(gateway));
        assert(permanentGatewayHandler(gateway) == reinterpret_cast<void *>(handler));

        std::thread held_call([&] {
            const int result = function(17);
            if (result != 18 && result != 1017) {
                std::abort();
            }
        });
        const std::uint64_t entry_deadline = ::GetTickCount64() + kTimeoutMs;
        while (entered() == 0 || permanentGatewayActive(gateway) == 0) {
            if (::GetTickCount64() >= entry_deadline) {
                std::cerr << "gateway-unload handler-entry timeout cycle=" << cycle
                          << " gate=" << permanentGatewayAdmissionOpen(gateway)
                          << " active=" << permanentGatewayActive(gateway)
                          << " generation=" << permanentGatewayGeneration(gateway)
                          << " handler=" << permanentGatewayHandler(gateway) << '\n';
                std::abort();
            }
            std::this_thread::yield();
        }

        std::atomic<bool> detach_finished{false};
        bool detach_ok = false;
        std::string detach_error;
        std::thread detacher([&] {
            detach_ok = detachPermanentGateway(gateway, kTimeoutMs, detach_error);
            detach_finished.store(true, std::memory_order_release);
        });

        ::Sleep(2);
        // At least one callback is deliberately stopped inside the unloadable
        // DLL. Detach must not become safe until that callback returns through
        // the process-lifetime gateway and decrements active there.
        assert(!detach_finished.load(std::memory_order_acquire));
        set_hold(0);
        held_call.join();
        detacher.join();
        if (!detach_ok) {
            std::cerr << "gateway-unload detach failed cycle=" << cycle
                      << " error=" << detach_error << '\n';
            std::abort();
        }
        assert(permanentGatewayActive(gateway) == 0);
        assert(!permanentGatewayAdmissionOpen(gateway));
        assert(permanentGatewayHandler(gateway) == nullptr);

        if (::FreeLibrary(module) == FALSE) {
            std::cerr << "gateway-unload FreeLibrary failed cycle=" << cycle
                      << " error=" << ::GetLastError() << '\n';
            std::abort();
        }
        if (::GetModuleHandleW(kHandlerName) != nullptr) {
            std::cerr << "gateway-unload handler DLL remained loaded cycle=" << cycle << '\n';
            std::abort();
        }

        // Allocation traffic keeps traversing the stable entry after the
        // unloadable image is gone. Any stale handler jump should fault here.
        for (int value = 0; value < 64; ++value) {
            assert(function(value) == value + 1);
        }

        PermanentGatewayHandle discovered;
        if (!discoverPermanentGateway(target.address(), discovered, error)) {
            std::cerr << "gateway-unload rediscovery failed cycle=" << cycle << " error=" << error << '\n';
            std::abort();
        }
        assert(discovered.gateway == stable_gateway);
        assert(discovered.state == stable_state);
        assert(discovered.original == stable_original);
        gateway = discovered;
        ++rediscoveries;

        if ((cycle + 1) % 25 == 0) {
            std::cerr << "stage=permanent-gateway-dll-unload progress=" << (cycle + 1)
                      << '/' << kUnloadCycles << " active=" << permanentGatewayActive(gateway)
                      << " worker_calls=" << worker_calls.load(std::memory_order_relaxed) << '\n';
        }
    }

    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }

    assert(permanentGatewayActive(gateway) == 0);
    assert(!permanentGatewayAdmissionOpen(gateway));
    assert(permanentGatewayHandler(gateway) == nullptr);
    assert(rediscoveries == kUnloadCycles);
    assert(worker_calls.load(std::memory_order_relaxed) != 0);

    std::cerr << "stage=permanent-gateway-dll-unload pass cycles=" << kUnloadCycles
              << " rediscoveries=" << rediscoveries
              << " worker_calls=" << worker_calls.load(std::memory_order_relaxed)
              << " permanent_rx_bytes=" << gateway.permanent_rx_bytes
              << " permanent_rw_bytes=" << gateway.permanent_rw_bytes << '\n';
    return 0;
}
