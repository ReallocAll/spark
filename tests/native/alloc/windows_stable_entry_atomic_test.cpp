#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_stable_entry_atomic_test.cpp is Windows-only"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::AtomicEntryHook;
using spark::stable_entry_experiment::atomicCompareExchange16;
using spark::stable_entry_experiment::cpuSupportsAtomic16;

namespace {

using SyntheticFn = int(__cdecl *)(int);
using MallocFn = void *(__cdecl *)(std::size_t);

std::atomic<void *> g_synthetic_trampoline{nullptr};
std::atomic<std::uint64_t> g_synthetic_active{0};
std::atomic<std::uint64_t> g_synthetic_hook_calls{0};
std::atomic<std::uint64_t> g_synthetic_stale_calls{0};

std::atomic<void *> g_malloc_trampoline{nullptr};
std::atomic<std::uint64_t> g_malloc_active{0};
std::atomic<std::uint64_t> g_malloc_hook_calls{0};

extern "C" __declspec(noinline) int __cdecl syntheticHook(int value) noexcept
{
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
        // lea eax,[rcx+1]; ret; nops. Keeping a NOP after the short function
        // gives funchook's bounded relocation engine a complete >=5-byte patch
        // window without splitting any instruction.
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0xC3, 0x90, 0x90, 0x90, 0x90,
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

void testCmpxchg16bNoTornObservers()
{
    if (!cpuSupportsAtomic16()) {
        std::cout << "CMPXCHG16B unavailable; atomic16 capability test skipped\n";
        return;
    }

    auto *memory = static_cast<std::uint8_t *>(
        ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
    std::cout << "CMPXCHG16B contention: no torn 16-byte observers\n";
}

void testSyntheticLifecycleStress()
{
    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    assert(function(41) == 42);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> worker_calls{0};
    constexpr std::size_t kWorkers = 8;
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&] {
            int value = 1;
            while (!stop.load(std::memory_order_acquire)) {
                const int result = function(value);
                if (result != value + 1) {
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    constexpr std::size_t kCycles = 1000;
    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        AtomicEntryHook hook;
        std::string error;
        if (!hook.prepare(target.address(), reinterpret_cast<void *>(&syntheticHook), error)) {
            std::cerr << "prepare failed at cycle " << cycle << ": " << error << '\n';
            std::abort();
        }
        g_synthetic_trampoline.store(hook.trampoline(), std::memory_order_release);
        if (!hook.install(error)) {
            std::cerr << "install failed at cycle " << cycle << ": " << error << '\n';
            std::abort();
        }
        for (int i = 0; i < 32; ++i) {
            assert(function(i) == i + 1);
        }
        if (!hook.restore(error)) {
            std::cerr << "restore failed at cycle " << cycle << ": " << error << '\n';
            std::abort();
        }
        if (!hook.proveQuiescence(g_synthetic_active, 5000, error)) {
            std::cerr << "quiescence failed at cycle " << cycle << ": " << error << '\n';
            std::abort();
        }
        if (!hook.destroy(error)) {
            std::cerr << "destroy failed at cycle " << cycle << ": " << error << '\n';
            std::abort();
        }
        g_synthetic_trampoline.store(nullptr, std::memory_order_release);
        assert(function(9) == 10);
    }

    stop.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }
    assert(g_synthetic_hook_calls.load(std::memory_order_relaxed) != 0);
    assert(g_synthetic_stale_calls.load(std::memory_order_relaxed) == 0);
    assert(worker_calls.load(std::memory_order_relaxed) != 0);
    std::cout << "synthetic stable-entry lifecycle cycles=" << kCycles
              << " worker_calls=" << worker_calls.load(std::memory_order_relaxed)
              << " hook_calls=" << g_synthetic_hook_calls.load(std::memory_order_relaxed) << '\n';
}

void testRealUcrtMallocEntry()
{
    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    assert(ucrt != nullptr);
    void *entry = reinterpret_cast<void *>(::GetProcAddress(ucrt, "malloc"));
    assert(entry != nullptr);
    std::cout << "ucrtbase!malloc entry=0x" << std::hex << reinterpret_cast<std::uintptr_t>(entry) << std::dec
              << " align8=" << ((reinterpret_cast<std::uintptr_t>(entry) & 7U) == 0)
              << " align16=" << ((reinterpret_cast<std::uintptr_t>(entry) & 15U) == 0) << '\n';

    AtomicEntryHook hook;
    std::string error;
    if (!hook.prepare(entry, reinterpret_cast<void *>(&mallocHook), error)) {
        std::cerr << "real UCRT malloc stable-entry prepare unavailable: " << error << '\n';
        std::abort();
    }
    g_malloc_trampoline.store(hook.trampoline(), std::memory_order_release);
    assert(hook.install(error));
    for (std::size_t i = 1; i <= 4096; i += 17) {
        void *pointer = std::malloc(i);
        assert(pointer != nullptr);
        std::free(pointer);
    }
    assert(hook.restore(error));
    assert(hook.proveQuiescence(g_malloc_active, 5000, error));
    assert(hook.destroy(error));
    g_malloc_trampoline.store(nullptr, std::memory_order_release);
    assert(g_malloc_hook_calls.load(std::memory_order_relaxed) != 0);
    std::cout << "real UCRT malloc stable-entry hook_calls=" << g_malloc_hook_calls.load(std::memory_order_relaxed)
              << '\n';
}

}  // namespace

int main()
{
    testCmpxchg16bNoTornObservers();
    testSyntheticLifecycleStress();
    testRealUcrtMallocEntry();
    std::cout << "Windows atomic stable-entry native tests passed\n";
    return 0;
}
