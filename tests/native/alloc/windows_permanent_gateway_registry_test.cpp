#include "native/alloc/windows_permanent_gateway_registry_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_registry_test.cpp is Windows-only"
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

using spark::stable_entry_experiment::acquirePermanentGateway;
using spark::stable_entry_experiment::bindPermanentGateway;
using spark::stable_entry_experiment::detachPermanentGateway;
using spark::stable_entry_experiment::permanentGatewayActive;
using spark::stable_entry_experiment::permanentGatewayAdmissionOpen;
using spark::stable_entry_experiment::PermanentGatewayHandle;
using spark::stable_entry_experiment::permanentGatewayHandler;
using spark::stable_entry_experiment::permanentGatewayOriginal;

namespace {

using TargetFn = int(__cdecl *)(int);
constexpr std::size_t kReloads = 1000;
constexpr std::uint64_t kTimeoutMs = 5000;

PermanentGatewayHandle g_gateway;
std::atomic<std::uint64_t> g_handler_calls{0};

extern "C" __declspec(noinline) int __cdecl registryHandler(int value) noexcept
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

[[nodiscard]] DWORD processHandleCount()
{
    DWORD count = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &count) == FALSE) {
        std::cerr << "GetProcessHandleCount failed error=" << ::GetLastError() << '\n';
        std::abort();
    }
    return count;
}

void replacePublicEntryWithThirdParty(void *entry)
{
    const std::array<std::uint8_t, 8> replacement = {
        0x8D, 0x41, 0x02,  // lea eax,[rcx+2]
        0x66, 0x90,        // nop
        0xC3,              // ret
        0x90, 0x90,
    };
    DWORD old_protection = 0;
    assert(::VirtualProtect(entry, replacement.size(), PAGE_EXECUTE_READWRITE, &old_protection) != FALSE);
    std::memcpy(entry, replacement.data(), replacement.size());
    assert(::FlushInstructionCache(::GetCurrentProcess(), entry, replacement.size()) != FALSE);
    DWORD ignored = 0;
    assert(::VirtualProtect(entry, replacement.size(), old_protection, &ignored) != FALSE);
}

}  // namespace

int main()
{
    std::cerr << "stage=permanent-gateway-registry begin reloads=" << kReloads << '\n';
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
    assert(public_entry(41) == 42);

    std::string error;
    if (!acquirePermanentGateway(target.address(), 0, g_gateway, error)) {
        std::cerr << "registry first acquire failed: " << error << '\n';
        return 2;
    }
    assert(g_gateway.gateway != nullptr && g_gateway.state != nullptr && g_gateway.original != nullptr);
    const void *stable_gateway = g_gateway.gateway;
    const void *stable_state = g_gateway.state;
    const void *stable_original = g_gateway.original;
    const DWORD handles_after_first = processHandleCount();

    for (std::size_t reload = 0; reload < kReloads; ++reload) {
        PermanentGatewayHandle reacquired;
        if (!acquirePermanentGateway(target.address(), 0, reacquired, error)) {
            std::cerr << "registry reuse failed reload=" << reload << " error=" << error << '\n';
            return 3;
        }
        assert(reacquired.gateway == stable_gateway);
        assert(reacquired.state == stable_state);
        assert(reacquired.original == stable_original);
    }
    const DWORD handles_after_reloads = processHandleCount();
    if (handles_after_reloads != handles_after_first) {
        std::cerr << "registry temporary handle leak baseline=" << handles_after_first
                  << " after=" << handles_after_reloads << '\n';
        return 4;
    }

    if (!bindPermanentGateway(g_gateway, reinterpret_cast<void *>(&registryHandler), kTimeoutMs, error)) {
        std::cerr << "registry bind failed: " << error << '\n';
        return 5;
    }
    assert(public_entry(7) == 1008);
    if (!detachPermanentGateway(g_gateway, kTimeoutMs, error)) {
        std::cerr << "registry detach failed: " << error << '\n';
        return 6;
    }
    assert(!permanentGatewayAdmissionOpen(g_gateway));
    assert(permanentGatewayActive(g_gateway) == 0);
    assert(permanentGatewayHandler(g_gateway) == nullptr);
    const std::uint64_t handler_calls_after_detach = g_handler_calls.load(std::memory_order_relaxed);

    auto cached_gateway = reinterpret_cast<TargetFn>(g_gateway.gateway);
    assert(cached_gateway(10) == 11);
    assert(g_handler_calls.load(std::memory_order_relaxed) == handler_calls_after_detach);

    // The synthetic third party takes ownership only while there are no callers
    // executing the public entry. Registry behavior after ownership loss is the
    // subject of this test; live unsynchronized patching remains unsupported.
    replacePublicEntryWithThirdParty(target.address());
    assert(public_entry(10) == 12);
    const DWORD handles_before_failed_reloads = processHandleCount();

    for (std::size_t reload = 0; reload < kReloads; ++reload) {
        PermanentGatewayHandle rejected;
        error.clear();
        if (acquirePermanentGateway(target.address(), 0, rejected, error)) {
            std::cerr << "registry unexpectedly reacquired lost entry ownership reload=" << reload << '\n';
            return 7;
        }
        if (error.empty()) {
            std::cerr << "registry ownership-loss rejection lacked diagnostic reload=" << reload << '\n';
            return 8;
        }
        assert(public_entry(10) == 12);
        assert(cached_gateway(10) == 11);
        assert(g_handler_calls.load(std::memory_order_relaxed) == handler_calls_after_detach);
    }

    const DWORD handles_after_failed_reloads = processHandleCount();
    if (handles_after_failed_reloads != handles_before_failed_reloads) {
        std::cerr << "registry ownership-loss path leaked temporary handles baseline=" << handles_before_failed_reloads
                  << " after=" << handles_after_failed_reloads << '\n';
        return 9;
    }
    assert(g_gateway.gateway == stable_gateway);
    assert(g_gateway.state == stable_state);
    assert(g_gateway.original == stable_original);
    assert(permanentGatewayActive(g_gateway) == 0);
    assert(permanentGatewayHandler(g_gateway) == nullptr);

    std::cerr << "stage=permanent-gateway-registry pass"
              << " successful_reloads=" << kReloads << " rejected_ownership_loss_reloads=" << kReloads
              << " permanent_handle_delta=1"
              << " handler_calls=" << handler_calls_after_detach << '\n';
    return 0;
}
