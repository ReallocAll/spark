#include "native/alloc/windows_permanent_iat_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_iat_gateway_unload_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using spark::permanent_iat_gateway_experiment::bindPermanentIatGateway;
using spark::permanent_iat_gateway_experiment::createPermanentIatGateway;
using spark::permanent_iat_gateway_experiment::detachPermanentIatGateway;
using spark::permanent_iat_gateway_experiment::discoverPermanentIatGateway;
using spark::permanent_iat_gateway_experiment::permanentIatGatewayActive;
using spark::permanent_iat_gateway_experiment::permanentIatGatewayAdmissionOpen;
using spark::permanent_iat_gateway_experiment::permanentIatGatewayGeneration;
using spark::permanent_iat_gateway_experiment::PermanentIatGatewayHandle;
using spark::permanent_iat_gateway_experiment::permanentIatGatewayHandler;

namespace {

using TargetFn = int(__cdecl *)(int);
using SetHoldFn = void(__cdecl *)(int);
using ResetEnteredFn = void(__cdecl *)();
using EnteredFn = int(__cdecl *)();

constexpr std::size_t kWorkers = 4;
constexpr std::size_t kUnloadCycles = 1000;
constexpr std::uint64_t kTimeoutMs = 5000;
constexpr int kHeldValue = 0x60000000;
constexpr wchar_t kHandlerName[] = L"windows_iat_gateway_test_handler.dll";

std::atomic<std::size_t> g_cycle{0};
std::atomic<unsigned> g_phase{0};
std::atomic<void *> g_gateway{nullptr};
std::atomic<std::uintptr_t> g_slot{0};

extern "C" __declspec(noinline) int __cdecl originalTarget(int value) noexcept
{
    return value + 1;
}

[[nodiscard]] std::uintptr_t addressOf(TargetFn function) noexcept
{
    return reinterpret_cast<std::uintptr_t>(function);
}

[[nodiscard]] TargetFn functionAt(std::uintptr_t address) noexcept
{
    return reinterpret_cast<TargetFn>(address);
}

[[noreturn]] void fail(const char *reason)
{
    std::fprintf(stderr,
                 "stage=permanent-iat-gateway-dll-unload failure=%s cycle=%zu phase=%u slot=0x%llx gateway=%p\n",
                 reason, g_cycle.load(std::memory_order_relaxed), g_phase.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_slot.load(std::memory_order_relaxed)),
                 g_gateway.load(std::memory_order_relaxed));
    std::fflush(stderr);
    std::abort();
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    const EXCEPTION_RECORD *record = exception != nullptr ? exception->ExceptionRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    const void *address = record != nullptr ? record->ExceptionAddress : nullptr;
    std::uintptr_t rip = 0;
#if defined(_M_X64)
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        rip = static_cast<std::uintptr_t>(exception->ContextRecord->Rip);
    }
#endif
    std::fprintf(stderr,
                 "stage=permanent-iat-gateway-dll-unload exception=0x%08lx address=%p rip=0x%llx cycle=%zu phase=%u "
                 "slot=0x%llx gateway=%p\n",
                 static_cast<unsigned long>(code), address, static_cast<unsigned long long>(rip),
                 g_cycle.load(std::memory_order_relaxed), g_phase.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_slot.load(std::memory_order_relaxed)),
                 g_gateway.load(std::memory_order_relaxed));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

[[nodiscard]] std::wstring handlerPath()
{
    wchar_t buffer[32768]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        fail("GetModuleFileNameW");
    }
    std::wstring path(buffer, length);
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        fail("handler-path");
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
        fail("GetProcAddress");
    }
    return reinterpret_cast<Function>(proc);
}

void requireKnownResult(int result, int value)
{
    if (result != value + 1 && result != value + 1000) {
        fail("unknown-call-result");
    }
}

}  // namespace

int main()
{
    ::SetUnhandledExceptionFilter(&unhandledExceptionFilter);
    std::fprintf(stderr, "stage=permanent-iat-gateway-dll-unload begin cycles=%zu workers=%zu\n", kUnloadCycles,
                 kWorkers);

    PermanentIatGatewayHandle gateway;
    std::string error;
    if (!createPermanentIatGateway(reinterpret_cast<void *>(&originalTarget), 0, gateway, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway-dll-unload create-failure error=%s\n", error.c_str());
        return 2;
    }
    g_gateway.store(gateway.gateway, std::memory_order_release);

    alignas(std::uintptr_t) std::uintptr_t slot_storage = addressOf(&originalTarget);
    std::atomic_ref<std::uintptr_t> slot(slot_storage);
    if (!slot.is_lock_free()) {
        fail("pointer-slot-not-lock-free");
    }
    std::uintptr_t expected = addressOf(&originalTarget);
    if (!slot.compare_exchange_strong(expected, reinterpret_cast<std::uintptr_t>(gateway.gateway),
                                      std::memory_order_acq_rel, std::memory_order_acquire)) {
        fail("initial-slot-publication");
    }
    g_slot.store(slot.load(std::memory_order_acquire), std::memory_order_release);
    const TargetFn cached_gateway = functionAt(reinterpret_cast<std::uintptr_t>(gateway.gateway));
    if (cached_gateway(41) != 42) {
        fail("initial-pass-through");
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
                TargetFn target = functionAt(slot.load(std::memory_order_acquire));
                requireKnownResult(target(value), value);
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t cycle = 0; cycle < kUnloadCycles; ++cycle) {
        g_cycle.store(cycle, std::memory_order_release);
        g_phase.store(1, std::memory_order_release);
        HMODULE module = ::LoadLibraryW(dll_path.c_str());
        if (module == nullptr) {
            std::fprintf(stderr, "stage=permanent-iat-gateway-dll-unload LoadLibrary-failure cycle=%zu error=%lu\n",
                         cycle, static_cast<unsigned long>(::GetLastError()));
            std::abort();
        }

        TargetFn handler = requiredExport<TargetFn>(module, "windowsGatewayTestHandler");
        SetHoldFn set_hold = requiredExport<SetHoldFn>(module, "windowsGatewayTestSetHold");
        ResetEnteredFn reset_special_entered =
            requiredExport<ResetEnteredFn>(module, "windowsGatewayTestResetSpecialEntered");
        EnteredFn special_entered = requiredExport<EnteredFn>(module, "windowsGatewayTestSpecialEntered");

        g_phase.store(2, std::memory_order_release);
        if (!bindPermanentIatGateway(gateway, reinterpret_cast<void *>(handler), kTimeoutMs, error)) {
            std::fprintf(stderr, "stage=permanent-iat-gateway-dll-unload bind-failure cycle=%zu error=%s\n", cycle,
                         error.c_str());
            std::abort();
        }
        if (!permanentIatGatewayAdmissionOpen(gateway) ||
            permanentIatGatewayHandler(gateway) != reinterpret_cast<void *>(handler)) {
            fail("bound-state-invariant");
        }

        // Worker calls can enter the same handler concurrently, so only the
        // sentinel held call is allowed to satisfy the admission oracle.
        reset_special_entered();
        set_hold(1);
        std::thread held_call([&] {
            const int result = cached_gateway(kHeldValue);
            if (result != kHeldValue + 1000) {
                fail("held-handler-result");
            }
        });
        const std::uint64_t entry_deadline = ::GetTickCount64() + kTimeoutMs;
        while (special_entered() == 0 || permanentIatGatewayActive(gateway) == 0) {
            if (::GetTickCount64() >= entry_deadline) {
                fail("handler-entry-timeout");
            }
            std::this_thread::yield();
        }

        g_phase.store(3, std::memory_order_release);
        std::atomic<bool> detach_finished{false};
        bool detach_ok = false;
        std::string detach_error;
        std::thread detacher([&] {
            detach_ok = detachPermanentIatGateway(gateway, kTimeoutMs, detach_error);
            detach_finished.store(true, std::memory_order_release);
        });

        ::Sleep(1);
        if (detach_finished.load(std::memory_order_acquire)) {
            fail("detach-finished-before-handler-return");
        }
        set_hold(0);
        held_call.join();
        detacher.join();
        if (!detach_ok) {
            std::fprintf(stderr, "stage=permanent-iat-gateway-dll-unload detach-failure cycle=%zu error=%s\n", cycle,
                         detach_error.c_str());
            std::abort();
        }
        if (permanentIatGatewayAdmissionOpen(gateway) || permanentIatGatewayHandler(gateway) != nullptr) {
            fail("detached-state-invariant");
        }

        g_phase.store(4, std::memory_order_release);
        if (::FreeLibrary(module) == FALSE) {
            fail("FreeLibrary");
        }
        if (::GetModuleHandleW(kHandlerName) != nullptr) {
            fail("handler-dll-remained-loaded");
        }

        // The public slot and cached callers still target permanent code after
        // the unloadable handler image has disappeared. Both paths must remain
        // deterministic pass-through and must never retain stale Spark reachability.
        for (int value = 0; value < 64; ++value) {
            if (functionAt(slot.load(std::memory_order_acquire))(value) != value + 1 ||
                cached_gateway(value) != value + 1) {
                fail("post-unload-pass-through");
            }
        }

        g_phase.store(5, std::memory_order_release);
        PermanentIatGatewayHandle discovered;
        if (!discoverPermanentIatGateway(gateway.gateway, discovered, error)) {
            std::fprintf(stderr, "stage=permanent-iat-gateway-dll-unload rediscovery-failure cycle=%zu error=%s\n",
                         cycle, error.c_str());
            std::abort();
        }
        if (discovered.gateway != stable_gateway || discovered.state != stable_state ||
            discovered.original != stable_original || permanentIatGatewayAdmissionOpen(discovered) ||
            permanentIatGatewayHandler(discovered) != nullptr) {
            fail("rediscovered-state-invariant");
        }
        gateway = discovered;

        if ((cycle + 1) % 50 == 0) {
            std::fprintf(stderr,
                         "stage=permanent-iat-gateway-dll-unload progress=%zu/%zu worker_calls=%llu generation=%llu "
                         "active=%llu rx=%zu rw=%zu\n",
                         cycle + 1, kUnloadCycles,
                         static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(permanentIatGatewayGeneration(gateway)),
                         static_cast<unsigned long long>(permanentIatGatewayActive(gateway)),
                         gateway.permanent_rx_bytes, gateway.permanent_rw_bytes);
            std::fflush(stderr);
        }
    }

    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }

    const std::uint64_t settle_deadline = ::GetTickCount64() + kTimeoutMs;
    while (permanentIatGatewayActive(gateway) != 0 && ::GetTickCount64() < settle_deadline) {
        std::this_thread::yield();
    }
    if (permanentIatGatewayActive(gateway) != 0 || permanentIatGatewayAdmissionOpen(gateway) ||
        permanentIatGatewayHandler(gateway) != nullptr || worker_calls.load(std::memory_order_relaxed) == 0) {
        fail("final-state-invariant");
    }

    std::fprintf(stderr,
                 "stage=permanent-iat-gateway-dll-unload pass cycles=%zu worker_calls=%llu generation=%llu "
                 "permanent_rx_bytes=%zu permanent_rw_bytes=%zu\n",
                 kUnloadCycles, static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(permanentIatGatewayGeneration(gateway)), gateway.permanent_rx_bytes,
                 gateway.permanent_rw_bytes);
    return 0;
}
