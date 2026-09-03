#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_stable_entry_atomic_test.cpp is Windows-only"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::atomicCompareExchange16;
using spark::stable_entry_experiment::atomicCompareExchange8;
using spark::stable_entry_experiment::AtomicEntryHook;
using spark::stable_entry_experiment::cpuSupportsAtomic16;

namespace {

using SyntheticFn = int(__cdecl *)(int);
using MallocFn = void *(__cdecl *)(std::size_t);

std::atomic<void *> g_synthetic_trampoline{nullptr};
std::atomic<std::uint64_t> g_synthetic_active{0};
std::atomic<std::uint64_t> g_synthetic_hook_calls{0};
std::atomic<std::uint64_t> g_synthetic_stale_calls{0};
std::atomic<bool> g_hold_pre_guard{false};
std::atomic<bool> g_pre_guard_entered{false};
std::atomic<std::uint64_t> g_stress_cycle{0};
std::atomic<unsigned> g_stress_phase{0};

std::atomic<void *> g_malloc_trampoline{nullptr};
std::atomic<std::uint64_t> g_malloc_active{0};
std::atomic<std::uint64_t> g_malloc_hook_calls{0};

LONG WINAPI stressUnhandledExceptionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    const DWORD code =
        exception != nullptr && exception->ExceptionRecord != nullptr ? exception->ExceptionRecord->ExceptionCode : 0;
    const void *address = exception != nullptr && exception->ExceptionRecord != nullptr
                            ? exception->ExceptionRecord->ExceptionAddress
                            : nullptr;
    std::uintptr_t instruction = 0;
#if defined(_M_X64)
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        instruction = static_cast<std::uintptr_t>(exception->ContextRecord->Rip);
    }
#endif
    std::fprintf(stderr,
                 "stage=synthetic-stress exception code=0x%08lx address=%p rip=0x%llx cycle=%llu phase=%u active=%llu "
                 "trampoline=%p\n",
                 static_cast<unsigned long>(code), address, static_cast<unsigned long long>(instruction),
                 static_cast<unsigned long long>(g_stress_cycle.load(std::memory_order_relaxed)),
                 g_stress_phase.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_synthetic_active.load(std::memory_order_relaxed)),
                 g_synthetic_trampoline.load(std::memory_order_relaxed));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(noinline) int __cdecl syntheticHook(int value) noexcept
{
    // Deliberately before the active counter. A teardown test holds a thread
    // here to prove that RIP quiescence closes the pre-guard corridor which an
    // active counter alone cannot observe.
    if (g_hold_pre_guard.load(std::memory_order_acquire)) {
        g_pre_guard_entered.store(true, std::memory_order_release);
        while (g_hold_pre_guard.load(std::memory_order_acquire)) {
            _mm_pause();
        }
    }

    g_synthetic_active.fetch_add(1, std::memory_order_acq_rel);
    auto *callable = reinterpret_cast<SyntheticFn>(g_synthetic_trampoline.load(std::memory_order_acquire));
    if (callable == nullptr) {
        g_synthetic_stale_calls.fetch_add(1, std::memory_order_relaxed);
        g_synthetic_active.fetch_sub(1, std::memory_order_release);
        return value + 1;
    }
    const int result = callable(value);
    g_synthetic_hook_calls.fetch_add(1, std::memory_order_relaxed);
    g_synthetic_active.fetch_sub(1, std::memory_order_release);
    return result;
}

extern "C" __declspec(noinline) void *__cdecl mallocHook(std::size_t size) noexcept
{
    g_malloc_active.fetch_add(1, std::memory_order_acq_rel);
    auto *callable = reinterpret_cast<MallocFn>(g_malloc_trampoline.load(std::memory_order_acquire));
    void *result = callable != nullptr ? callable(size) : nullptr;
    g_malloc_hook_calls.fetch_add(1, std::memory_order_relaxed);
    g_malloc_active.fetch_sub(1, std::memory_order_release);
    return result;
}

class ExecutableSyntheticFunction {
public:
    ExecutableSyntheticFunction()
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(page_ != nullptr);
        assert((reinterpret_cast<std::uintptr_t>(page_) & 7U) == 0);
        // Five complete bytes precede RET. This exercises funchook's normal
        // bounded relocation path instead of its special short-function tail.
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(page_, code.data(), code.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, code.size()) != FALSE);
        DWORD old = 0;
        assert(::VirtualProtect(page_, 64 * 1024, PAGE_EXECUTE_READ, &old) != FALSE);
    }

    ~ExecutableSyntheticFunction()
    {
        if (page_ != nullptr) {
            ::VirtualFree(page_, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(page_); }
    [[nodiscard]] void *address() const noexcept { return page_; }

private:
    std::uint8_t *page_ = nullptr;
};

bool same16(const std::array<std::uint8_t, 16> &a, const std::array<std::uint8_t, 16> &b) noexcept
{
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

bool sameEntry8(void *entry, const std::array<std::uint8_t, 16> &bytes) noexcept
{
    return entry != nullptr && std::memcmp(entry, bytes.data(), 8) == 0;
}

bool exchangeExecutable8(void *entry, const std::array<std::uint8_t, 16> &expected,
                         const std::array<std::uint8_t, 16> &desired) noexcept
{
    DWORD old_protection = 0;
    if (::VirtualProtect(entry, 8, PAGE_EXECUTE_READWRITE, &old_protection) == FALSE) {
        return false;
    }
    const bool exchanged = atomicCompareExchange8(entry, expected, desired).exchanged;
    const BOOL flushed = ::FlushInstructionCache(::GetCurrentProcess(), entry, 8);
    DWORD ignored = 0;
    const BOOL restored = ::VirtualProtect(entry, 8, old_protection, &ignored);
    return exchanged && flushed != FALSE && restored != FALSE;
}

std::array<std::uint8_t, 16> thirdPartyBytes(const std::array<std::uint8_t, 16> &owned) noexcept
{
    auto third_party = owned;
    third_party[0] ^= 0x5A;
    if (third_party[0] == owned[0]) {
        third_party[0] ^= 0xA5;
    }
    return third_party;
}

void testCmpxchg16bNoTornObservers()
{
    std::cerr << "stage=cmpxchg16 begin\n";
    if (!cpuSupportsAtomic16()) {
        std::cerr << "stage=cmpxchg16 skipped: CPU feature unavailable\n";
        return;
    }

    auto *memory =
        static_cast<std::uint8_t *>(::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(memory != nullptr);
    assert((reinterpret_cast<std::uintptr_t>(memory) & 15U) == 0);

    std::array<std::uint8_t, 16> a{};
    std::array<std::uint8_t, 16> b{};
    std::array<std::uint8_t, 16> impossible{};
    a.fill(0xA5);
    b.fill(0x5A);
    impossible.fill(0xCC);
    std::memcpy(memory, a.data(), a.size());

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto observed = atomicCompareExchange16(memory, impossible, impossible).observed;
            if (!same16(observed, a) && !same16(observed, b)) {
                torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    for (std::size_t i = 0; i < 200000; ++i) {
        const auto &from = (i & 1U) == 0 ? a : b;
        const auto &to = (i & 1U) == 0 ? b : a;
        while (!atomicCompareExchange16(memory, from, to).exchanged) {
        }
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    assert(torn.load(std::memory_order_relaxed) == 0);
    ::VirtualFree(memory, 0, MEM_RELEASE);
    std::cerr << "stage=cmpxchg16 pass torn=0\n";
}

void runSyntheticCycle(ExecutableSyntheticFunction &target, std::size_t cycle)
{
    SyntheticFn function = target.function();
    AtomicEntryHook hook;
    std::string error;
    g_stress_cycle.store(cycle, std::memory_order_release);
    g_stress_phase.store(1, std::memory_order_release);  // prepare
    if (!hook.prepare(target.address(), reinterpret_cast<void *>(&syntheticHook), error)) {
        std::cerr << "prepare failed at cycle " << cycle << ": " << error << '\n';
        std::abort();
    }
    g_synthetic_trampoline.store(hook.trampoline(), std::memory_order_release);
    g_stress_phase.store(2, std::memory_order_release);  // install transaction
    if (!hook.install(error)) {
        std::cerr << "install failed at cycle " << cycle << ": " << error << '\n';
        std::abort();
    }
    g_stress_phase.store(3, std::memory_order_release);  // installed calls
    for (int i = 0; i < 32; ++i) {
        assert(function(i) == i + 1);
    }
    g_stress_phase.store(4, std::memory_order_release);  // restore transaction
    if (!hook.restore(error)) {
        std::cerr << "restore failed at cycle " << cycle << ": " << error << '\n';
        std::abort();
    }
    g_stress_phase.store(5, std::memory_order_release);  // post-restore quiescence
    if (!hook.proveQuiescence(g_synthetic_active, 5000, error)) {
        std::cerr << "quiescence failed at cycle " << cycle << ": " << error << '\n';
        std::abort();
    }
    g_stress_phase.store(6, std::memory_order_release);  // destroy
    if (!hook.destroy(error)) {
        std::cerr << "destroy failed at cycle " << cycle << ": " << error << '\n';
        std::abort();
    }
    g_synthetic_trampoline.store(nullptr, std::memory_order_release);
    g_stress_phase.store(7, std::memory_order_release);  // restored baseline
    assert(function(9) == 10);
}

void testSyntheticSingleThreadLifecycle()
{
    std::cerr << "stage=synthetic-single begin\n";
    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    assert(function(41) == 42);
    runSyntheticCycle(target, 0);
    assert(g_synthetic_hook_calls.load(std::memory_order_relaxed) != 0);
    assert(g_synthetic_stale_calls.load(std::memory_order_relaxed) == 0);
    std::cerr << "stage=synthetic-single pass hook_calls=" << g_synthetic_hook_calls.load(std::memory_order_relaxed)
              << '\n';
}

void testSyntheticLifecycleStress()
{
    std::cerr << "stage=synthetic-stress begin\n";
    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    assert(function(41) == 42);

    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = ::SetUnhandledExceptionFilter(&stressUnhandledExceptionFilter);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> worker_calls{0};
    constexpr std::size_t kWorkers = 8;
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = 1;
            while (!stop.load(std::memory_order_acquire)) {
                const int result = function(value);
                if (result != value + 1) {
                    std::fprintf(stderr,
                                 "stage=synthetic-stress mismatch worker=%llu cycle=%llu phase=%u value=%d result=%d "
                                 "active=%llu trampoline=%p\n",
                                 static_cast<unsigned long long>(worker),
                                 static_cast<unsigned long long>(g_stress_cycle.load(std::memory_order_relaxed)),
                                 g_stress_phase.load(std::memory_order_relaxed), value, result,
                                 static_cast<unsigned long long>(g_synthetic_active.load(std::memory_order_relaxed)),
                                 g_synthetic_trampoline.load(std::memory_order_relaxed));
                    std::fflush(stderr);
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    constexpr std::size_t kCycles = 1000;
    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        runSyntheticCycle(target, cycle);
        if ((cycle + 1) % 25 == 0) {
            std::cerr << "stage=synthetic-stress progress=" << (cycle + 1) << '/' << kCycles << '\n';
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }
    (void)::SetUnhandledExceptionFilter(previous_filter);
    assert(g_synthetic_hook_calls.load(std::memory_order_relaxed) != 0);
    assert(g_synthetic_stale_calls.load(std::memory_order_relaxed) == 0);
    assert(worker_calls.load(std::memory_order_relaxed) != 0);
    std::cerr << "stage=synthetic-stress pass cycles=" << kCycles
              << " worker_calls=" << worker_calls.load(std::memory_order_relaxed)
              << " hook_calls=" << g_synthetic_hook_calls.load(std::memory_order_relaxed) << '\n';
}

void testInstallOwnershipLoss()
{
    std::cerr << "stage=ownership-install begin\n";
    ExecutableSyntheticFunction target;
    AtomicEntryHook hook;
    std::string error;
    assert(hook.prepare(target.address(), reinterpret_cast<void *>(&syntheticHook), error));
    const auto original = hook.originalBytes();
    const auto third_party = thirdPartyBytes(original);
    assert(exchangeExecutable8(target.address(), original, third_party));

    assert(!hook.install(error));
    assert(hook.unsafe());
    assert(error.find("ownership lost") != std::string::npos);
    assert(sameEntry8(target.address(), third_party));

    // Test cleanup is deliberately external to Spark. The assertion above proves
    // Spark did not overwrite the third-party owner when its CAS failed.
    assert(exchangeExecutable8(target.address(), third_party, original));
    std::cerr << "stage=ownership-install pass error=" << error << '\n';
}

void testRestoreOwnershipLoss()
{
    std::cerr << "stage=ownership-restore begin\n";
    ExecutableSyntheticFunction target;
    AtomicEntryHook hook;
    std::string error;
    assert(hook.prepare(target.address(), reinterpret_cast<void *>(&syntheticHook), error));
    const auto original = hook.originalBytes();
    g_synthetic_trampoline.store(hook.trampoline(), std::memory_order_release);
    assert(hook.install(error));
    const auto installed = hook.installedBytes();
    const auto third_party = thirdPartyBytes(installed);
    assert(exchangeExecutable8(target.address(), installed, third_party));

    assert(!hook.restore(error));
    assert(hook.unsafe());
    assert(error.find("ownership lost") != std::string::npos);
    assert(sameEntry8(target.address(), third_party));

    // External cleanup after proving fail-closed ownership. Unsafe hook resources
    // intentionally remain leaked until this isolated process exits.
    assert(exchangeExecutable8(target.address(), third_party, original));
    g_synthetic_trampoline.store(nullptr, std::memory_order_release);
    std::cerr << "stage=ownership-restore pass error=" << error << '\n';
}

void testPreGuardRipBlocksQuiescence()
{
    std::cerr << "stage=pre-guard-rip begin\n";
    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    AtomicEntryHook hook;
    std::string error;
    assert(hook.prepare(target.address(), reinterpret_cast<void *>(&syntheticHook), error));
    g_synthetic_trampoline.store(hook.trampoline(), std::memory_order_release);
    assert(hook.install(error));

    g_pre_guard_entered.store(false, std::memory_order_release);
    g_hold_pre_guard.store(true, std::memory_order_release);
    std::thread worker([&] {
        const int result = function(77);
        if (result != 78) {
            std::abort();
        }
    });

    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!g_pre_guard_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= entered_deadline) {
            std::abort();
        }
        ::Sleep(1);
    }
    assert(g_synthetic_active.load(std::memory_order_acquire) == 0);
    assert(hook.restore(error));

    // active==0 is intentionally true here. The only reason quiescence may not
    // succeed is the live RIP inside the Spark hook before guard acquisition.
    assert(!hook.proveQuiescence(g_synthetic_active, 100, error));
    assert(hook.unsafe());
    assert(error.find("executable ranges") != std::string::npos);

    g_hold_pre_guard.store(false, std::memory_order_release);
    worker.join();
    g_synthetic_trampoline.store(nullptr, std::memory_order_release);
    assert(function(9) == 10);
    std::cerr << "stage=pre-guard-rip pass error=" << error << '\n';
}

void testDynamicCodePolicyFailClosed()
{
    std::cerr << "stage=dynamic-code begin\n";
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy{};
    policy.ProhibitDynamicCode = 1;
    if (::SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &policy, sizeof(policy)) == FALSE) {
        std::cerr << "SetProcessMitigationPolicy failed error=" << ::GetLastError() << '\n';
        std::abort();
    }

    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    assert(ucrt != nullptr);
    void *entry = reinterpret_cast<void *>(::GetProcAddress(ucrt, "malloc"));
    assert(entry != nullptr);

    AtomicEntryHook hook;
    std::string error;
    assert(!hook.prepare(entry, reinterpret_cast<void *>(&mallocHook), error));
    assert(hook.unsafe());
    assert(error.find("ProcessDynamicCodePolicy") != std::string::npos);
    std::cerr << "stage=dynamic-code pass error=" << error << '\n';
}

void testRealUcrtMallocEntry()
{
    std::cerr << "stage=ucrt-malloc begin\n";
    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    assert(ucrt != nullptr);
    void *entry = reinterpret_cast<void *>(::GetProcAddress(ucrt, "malloc"));
    assert(entry != nullptr);
    std::cerr << "ucrtbase!malloc entry=0x" << std::hex << reinterpret_cast<std::uintptr_t>(entry) << std::dec
              << " align8=" << ((reinterpret_cast<std::uintptr_t>(entry) & 7U) == 0)
              << " align16=" << ((reinterpret_cast<std::uintptr_t>(entry) & 15U) == 0) << '\n';

    AtomicEntryHook hook;
    std::string error;
    if (!hook.prepare(entry, reinterpret_cast<void *>(&mallocHook), error)) {
        std::cerr << "real UCRT malloc stable-entry prepare unavailable: " << error << '\n';
        std::abort();
    }
    g_malloc_trampoline.store(hook.trampoline(), std::memory_order_release);
    if (!hook.install(error)) {
        std::cerr << "real UCRT malloc install failed: " << error << '\n';
        std::abort();
    }
    for (std::size_t i = 1; i <= 4096; i += 17) {
        void *pointer = std::malloc(i);
        assert(pointer != nullptr);
        std::free(pointer);
    }
    if (!hook.restore(error)) {
        std::cerr << "real UCRT malloc restore failed: " << error << '\n';
        std::abort();
    }
    if (!hook.proveQuiescence(g_malloc_active, 5000, error)) {
        std::cerr << "real UCRT malloc quiescence failed: " << error << '\n';
        std::abort();
    }
    if (!hook.destroy(error)) {
        std::cerr << "real UCRT malloc destroy failed: " << error << '\n';
        std::abort();
    }
    g_malloc_trampoline.store(nullptr, std::memory_order_release);
    assert(g_malloc_hook_calls.load(std::memory_order_relaxed) != 0);
    std::cerr << "stage=ucrt-malloc pass hook_calls=" << g_malloc_hook_calls.load(std::memory_order_relaxed) << '\n';
}

}  // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "cmpxchg16" || mode == "all") {
        testCmpxchg16bNoTornObservers();
    }
    if (mode == "synthetic-single" || mode == "all") {
        testSyntheticSingleThreadLifecycle();
    }
    if (mode == "synthetic-stress" || mode == "all") {
        testSyntheticLifecycleStress();
    }
    if (mode == "ownership-install") {
        testInstallOwnershipLoss();
    }
    if (mode == "ownership-restore") {
        testRestoreOwnershipLoss();
    }
    if (mode == "pre-guard-rip") {
        testPreGuardRipBlocksQuiescence();
    }
    if (mode == "dynamic-code") {
        testDynamicCodePolicyFailClosed();
    }
    if (mode == "ucrt-malloc" || mode == "all") {
        testRealUcrtMallocEntry();
    }
    if (mode != "all" && mode != "cmpxchg16" && mode != "synthetic-single" && mode != "synthetic-stress" &&
        mode != "ownership-install" && mode != "ownership-restore" && mode != "pre-guard-rip" &&
        mode != "dynamic-code" && mode != "ucrt-malloc") {
        std::cerr << "unknown test mode: " << mode << '\n';
        return 2;
    }
    std::cerr << "Windows atomic stable-entry mode passed: " << mode << '\n';
    return 0;
}
