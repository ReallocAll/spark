#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_stable_entry_lifecycle_probe.cpp is Windows-only"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

using spark::stable_entry_experiment::AtomicEntryHook;

namespace {

using SyntheticFn = int(__cdecl *)(int);
std::atomic<void *> g_trampoline{nullptr};
std::atomic<std::uint64_t> g_active{0};
std::atomic<std::uint64_t> g_calls{0};

extern "C" __declspec(noinline) int __cdecl lifecycleHook(int value) noexcept
{
    g_active.fetch_add(1, std::memory_order_acq_rel);
    auto *callable = reinterpret_cast<SyntheticFn>(g_trampoline.load(std::memory_order_acquire));
    const int result = callable != nullptr ? callable(value) : -9999;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_active.fetch_sub(1, std::memory_order_release);
    return result;
}

class SyntheticCode {
public:
    SyntheticCode()
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(page_ != nullptr);
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0xC3, 0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(page_, code.data(), code.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, code.size()) != FALSE);
        DWORD old_protection = 0;
        assert(::VirtualProtect(page_, 64 * 1024, PAGE_EXECUTE_READ, &old_protection) != FALSE);
    }

    ~SyntheticCode()
    {
        if (page_ != nullptr) {
            ::VirtualFree(page_, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] void *address() const noexcept { return page_; }
    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(page_); }

private:
    std::uint8_t *page_ = nullptr;
};

}  // namespace

int main()
{
    SyntheticCode target;
    SyntheticFn function = target.function();
    assert(function(10) == 11);
    std::cerr << "lifecycle-probe: baseline-pass target=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(target.address()) << std::dec << '\n';

    AtomicEntryHook hook;
    std::string error;
    std::cerr << "lifecycle-probe: prepare-enter\n";
    if (!hook.prepare(target.address(), reinterpret_cast<void *>(&lifecycleHook), error)) {
        std::cerr << "lifecycle-probe: prepare-fail: " << error << '\n';
        return 2;
    }
    std::cerr << "lifecycle-probe: prepare-pass trampoline=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(hook.trampoline()) << " relay=0x"
              << reinterpret_cast<std::uintptr_t>(hook.relay()) << std::dec << '\n';
    g_trampoline.store(hook.trampoline(), std::memory_order_release);

    std::cerr << "lifecycle-probe: direct-hook-enter\n";
    const int direct_hook_result = lifecycleHook(12);
    std::cerr << "lifecycle-probe: direct-hook-return result=" << direct_hook_result
              << " calls=" << g_calls.load(std::memory_order_relaxed) << '\n';
    assert(direct_hook_result == 13);

    std::cerr << "lifecycle-probe: direct-relay-enter\n";
    auto *relay = reinterpret_cast<SyntheticFn>(hook.relay());
    const int direct_relay_result = relay(14);
    std::cerr << "lifecycle-probe: direct-relay-return result=" << direct_relay_result
              << " calls=" << g_calls.load(std::memory_order_relaxed) << '\n';
    assert(direct_relay_result == 15);

    std::cerr << "lifecycle-probe: install-enter\n";
    if (!hook.install(error)) {
        std::cerr << "lifecycle-probe: install-fail: " << error << '\n';
        return 3;
    }
    std::cerr << "lifecycle-probe: install-pass\n";

    std::cerr << "lifecycle-probe: patched-entry-call-enter\n";
    const int hooked_result = function(20);
    std::cerr << "lifecycle-probe: patched-entry-call-return result=" << hooked_result
              << " calls=" << g_calls.load(std::memory_order_relaxed) << '\n';
    assert(hooked_result == 21);
    assert(g_calls.load(std::memory_order_relaxed) == 3);

    std::cerr << "lifecycle-probe: restore-enter\n";
    if (!hook.restore(error)) {
        std::cerr << "lifecycle-probe: restore-fail: " << error << '\n';
        return 4;
    }
    std::cerr << "lifecycle-probe: restore-pass\n";

    std::cerr << "lifecycle-probe: quiescence-enter\n";
    if (!hook.proveQuiescence(g_active, 5000, error)) {
        std::cerr << "lifecycle-probe: quiescence-fail: " << error << '\n';
        return 5;
    }
    std::cerr << "lifecycle-probe: quiescence-pass\n";

    std::cerr << "lifecycle-probe: destroy-enter\n";
    if (!hook.destroy(error)) {
        std::cerr << "lifecycle-probe: destroy-fail: " << error << '\n';
        return 6;
    }
    g_trampoline.store(nullptr, std::memory_order_release);
    std::cerr << "lifecycle-probe: destroy-pass\n";
    assert(function(30) == 31);
    std::cerr << "lifecycle-probe: final-baseline-pass\n";
    return 0;
}
