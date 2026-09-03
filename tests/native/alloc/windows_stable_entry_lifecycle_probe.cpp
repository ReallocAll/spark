#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_stable_entry_lifecycle_probe.cpp is Windows-only"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iomanip>
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
        // Five complete bytes precede RET so funchook takes its normal bounded
        // relocation path instead of the unusual <5-byte short-function fallback.
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
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

void dumpMemory(const char *label, const void *address)
{
    MEMORY_BASIC_INFORMATION memory{};
    const SIZE_T queried = ::VirtualQuery(address, &memory, sizeof(memory));
    std::cerr << label << ": address=0x" << std::hex << reinterpret_cast<std::uintptr_t>(address) << std::dec
              << " query=" << queried;
    if (queried != 0) {
        std::cerr << " base=0x" << std::hex << reinterpret_cast<std::uintptr_t>(memory.BaseAddress) << " allocbase=0x"
                  << reinterpret_cast<std::uintptr_t>(memory.AllocationBase) << std::dec
                  << " size=" << memory.RegionSize << " state=0x" << std::hex << memory.State << " protect=0x"
                  << memory.Protect << " allocprotect=0x" << memory.AllocationProtect << std::dec;
    }
    std::cerr << " bytes=";
    const auto *bytes = static_cast<const std::uint8_t *>(address);
    for (std::size_t i = 0; i < 32; ++i) {
        std::cerr << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]);
    }
    std::cerr << std::dec << '\n';
}

void dumpMitigations()
{
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
    const BOOL cfg_ok = ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg,
                                                     static_cast<SIZE_T>(sizeof(cfg)));
    std::cerr << "mitigation: cfg_query=" << (cfg_ok != FALSE) << " cfg_enable=" << cfg.EnableControlFlowGuard
              << " cfg_strict=" << cfg.StrictMode << " cfg_export_suppression=" << cfg.EnableExportSuppression << '\n';

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamic{};
    const BOOL dynamic_ok = ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &dynamic,
                                                         static_cast<SIZE_T>(sizeof(dynamic)));
    std::cerr << "mitigation: dynamic_query=" << (dynamic_ok != FALSE)
              << " prohibit_dynamic=" << dynamic.ProhibitDynamicCode
              << " allow_thread_optout=" << dynamic.AllowThreadOptOut << '\n';
}

int callTrampolineWithSeh(SyntheticFn function, int value, DWORD &exception_code) noexcept
{
    exception_code = ERROR_SUCCESS;
    __try {
        return function(value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        exception_code = static_cast<DWORD>(::GetExceptionCode());
        return -123456;
    }
}

}  // namespace

int main()
{
    dumpMitigations();
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
    dumpMemory("trampoline", hook.trampoline());
    dumpMemory("relay", hook.relay());

    DWORD trampoline_exception = ERROR_SUCCESS;
    std::cerr << "lifecycle-probe: direct-trampoline-enter\n";
    const int direct_trampoline =
        callTrampolineWithSeh(reinterpret_cast<SyntheticFn>(hook.trampoline()), 11, trampoline_exception);
    std::cerr << "lifecycle-probe: direct-trampoline-return result=" << direct_trampoline << " exception=0x" << std::hex
              << trampoline_exception << std::dec << '\n';
    if (trampoline_exception != ERROR_SUCCESS || direct_trampoline != 12) {
        return 7;
    }

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
