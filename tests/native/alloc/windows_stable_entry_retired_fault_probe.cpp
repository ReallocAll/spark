#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_stable_entry_retired_fault_probe.cpp is Windows-only"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::AddressRange;
using spark::stable_entry_experiment::AtomicEntryHook;

namespace {

using SyntheticFn = int(__cdecl *)(int);

constexpr std::size_t kHistorySlots = 1024;
constexpr std::size_t kWorkers = 8;
constexpr std::size_t kCycles = 1000;

struct PublishedGeneration {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uintptr_t> relay_begin{0};
    std::atomic<std::uintptr_t> relay_end{0};
    std::atomic<std::uintptr_t> trampoline_begin{0};
    std::atomic<std::uintptr_t> trampoline_end{0};
    std::atomic<std::uintptr_t> hook_begin{0};
    std::atomic<std::uintptr_t> hook_end{0};
};

std::array<PublishedGeneration, kHistorySlots> g_history{};
std::atomic<std::uint64_t> g_generation{0};
std::atomic<std::uint64_t> g_cycle{0};
std::atomic<unsigned> g_phase{0};
std::atomic<std::uint64_t> g_active{0};
std::atomic<std::uint64_t> g_hook_calls{0};
std::atomic<std::uint64_t> g_stale_calls{0};
std::atomic<void *> g_trampoline{nullptr};
std::atomic<void *> g_relay{nullptr};
std::atomic<void *> g_entry{nullptr};

[[nodiscard]] bool contains(std::uintptr_t address, std::uintptr_t begin, std::uintptr_t end) noexcept
{
    return begin < end && address >= begin && address < end;
}

struct FaultClassification {
    const char *kind = "other";
    std::uint64_t generation = 0;
};

[[nodiscard]] FaultClassification classifyFaultRip(std::uintptr_t rip) noexcept
{
    FaultClassification best{};
    const std::uint64_t current = g_generation.load(std::memory_order_acquire);
    for (const PublishedGeneration &slot : g_history) {
        const std::uint64_t generation = slot.generation.load(std::memory_order_acquire);
        if (generation == 0) {
            continue;
        }
        const std::uintptr_t relay_begin = slot.relay_begin.load(std::memory_order_relaxed);
        const std::uintptr_t relay_end = slot.relay_end.load(std::memory_order_relaxed);
        const std::uintptr_t trampoline_begin = slot.trampoline_begin.load(std::memory_order_relaxed);
        const std::uintptr_t trampoline_end = slot.trampoline_end.load(std::memory_order_relaxed);
        const std::uintptr_t hook_begin = slot.hook_begin.load(std::memory_order_relaxed);
        const std::uintptr_t hook_end = slot.hook_end.load(std::memory_order_relaxed);

        const char *kind = nullptr;
        if (contains(rip, relay_begin, relay_end)) {
            kind = generation == current ? "current-relay" : "retired-relay";
        }
        else if (contains(rip, trampoline_begin, trampoline_end)) {
            kind = generation == current ? "current-trampoline" : "retired-trampoline";
        }
        else if (contains(rip, hook_begin, hook_end)) {
            kind = generation == current ? "current-hook" : "retired-generation-hook";
        }
        if (kind != nullptr && generation >= best.generation) {
            best.kind = kind;
            best.generation = generation;
        }
    }
    return best;
}

[[nodiscard]] const char *accessKind(ULONG_PTR operation) noexcept
{
    switch (operation) {
    case 0:
        return "read";
    case 1:
        return "write";
    case 8:
        return "execute";
    default:
        return "unknown";
    }
}

LONG WINAPI stressUnhandledExceptionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    const EXCEPTION_RECORD *record = exception != nullptr ? exception->ExceptionRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    const void *exception_address = record != nullptr ? record->ExceptionAddress : nullptr;

    std::uintptr_t rip = 0;
#if defined(_M_X64)
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        rip = static_cast<std::uintptr_t>(exception->ContextRecord->Rip);
    }
#endif

    ULONG_PTR av_operation = static_cast<ULONG_PTR>(~static_cast<ULONG_PTR>(0));
    ULONG_PTR av_target = 0;
    if (record != nullptr && code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        av_operation = record->ExceptionInformation[0];
        av_target = record->ExceptionInformation[1];
    }

    const FaultClassification classification = classifyFaultRip(rip);
    MEMORY_BASIC_INFORMATION memory{};
    const std::uintptr_t query_address = av_target != 0 ? static_cast<std::uintptr_t>(av_target) : rip;
    const SIZE_T queried =
        query_address != 0 ? ::VirtualQuery(reinterpret_cast<const void *>(query_address), &memory, sizeof(memory)) : 0;

    std::fprintf(
        stderr,
        "stage=retired-fault-probe exception code=0x%08lx exception_address=%p rip=0x%llx av_op=%llu av_kind=%s "
        "av_target=0x%llx class=%s matched_generation=%llu current_generation=%llu cycle=%llu phase=%u active=%llu "
        "entry=%p relay=%p trampoline=%p handler=%p query=%llu mem_base=%p alloc_base=%p region=%llu state=0x%lx "
        "protect=0x%lx alloc_protect=0x%lx\n",
        static_cast<unsigned long>(code), exception_address, static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(av_operation), accessKind(av_operation),
        static_cast<unsigned long long>(av_target), classification.kind,
        static_cast<unsigned long long>(classification.generation),
        static_cast<unsigned long long>(g_generation.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
        g_phase.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_active.load(std::memory_order_relaxed)),
        g_entry.load(std::memory_order_relaxed), g_relay.load(std::memory_order_relaxed),
        g_trampoline.load(std::memory_order_relaxed), reinterpret_cast<void *>(&stressUnhandledExceptionFilter),
        static_cast<unsigned long long>(queried), queried != 0 ? memory.BaseAddress : nullptr,
        queried != 0 ? memory.AllocationBase : nullptr,
        static_cast<unsigned long long>(queried != 0 ? memory.RegionSize : 0),
        static_cast<unsigned long>(queried != 0 ? memory.State : 0),
        static_cast<unsigned long>(queried != 0 ? memory.Protect : 0),
        static_cast<unsigned long>(queried != 0 ? memory.AllocationProtect : 0));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(noinline) int __cdecl syntheticHook(int value) noexcept
{
    g_active.fetch_add(1, std::memory_order_acq_rel);
    auto *callable = reinterpret_cast<SyntheticFn>(g_trampoline.load(std::memory_order_acquire));
    if (callable == nullptr) {
        g_stale_calls.fetch_add(1, std::memory_order_relaxed);
        g_active.fetch_sub(1, std::memory_order_release);
        return value + 1;
    }
    const int result = callable(value);
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    g_active.fetch_sub(1, std::memory_order_release);
    return result;
}

class ExecutableSyntheticFunction {
public:
    ExecutableSyntheticFunction()
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(page_ != nullptr);
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(page_, code.data(), code.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, code.size()) != FALSE);
        DWORD old_protection = 0;
        assert(::VirtualProtect(page_, 64 * 1024, PAGE_EXECUTE_READ, &old_protection) != FALSE);
    }

    ~ExecutableSyntheticFunction()
    {
        if (page_ != nullptr) {
            (void)::VirtualFree(page_, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(page_); }
    [[nodiscard]] void *address() const noexcept { return page_; }

private:
    std::uint8_t *page_ = nullptr;
};

void publishGeneration(std::uint64_t generation, AtomicEntryHook &hook)
{
    const auto ranges = hook.protectedRanges();
    assert(ranges.size() >= 3);
    PublishedGeneration &slot = g_history[(generation - 1) % kHistorySlots];
    slot.relay_begin.store(ranges[0].begin, std::memory_order_relaxed);
    slot.relay_end.store(ranges[0].end, std::memory_order_relaxed);
    slot.trampoline_begin.store(ranges[1].begin, std::memory_order_relaxed);
    slot.trampoline_end.store(ranges[1].end, std::memory_order_relaxed);
    slot.hook_begin.store(ranges[2].begin, std::memory_order_relaxed);
    slot.hook_end.store(ranges[2].end, std::memory_order_relaxed);
    slot.generation.store(generation, std::memory_order_release);
    g_generation.store(generation, std::memory_order_release);
}

void runCycle(ExecutableSyntheticFunction &target, std::size_t cycle)
{
    AtomicEntryHook hook;
    std::string error;
    const std::uint64_t generation = static_cast<std::uint64_t>(cycle) + 1;
    g_cycle.store(cycle, std::memory_order_release);
    g_phase.store(1, std::memory_order_release);
    g_entry.store(target.address(), std::memory_order_release);

    if (!hook.prepare(target.address(), reinterpret_cast<void *>(&syntheticHook), error)) {
        std::cerr << "retired-fault-probe prepare failed cycle=" << cycle << " error=" << error << '\n';
        std::abort();
    }
    g_trampoline.store(hook.trampoline(), std::memory_order_release);
    g_relay.store(hook.relay(), std::memory_order_release);
    publishGeneration(generation, hook);

    g_phase.store(2, std::memory_order_release);
    if (!hook.install(error)) {
        std::cerr << "retired-fault-probe install failed cycle=" << cycle << " error=" << error << '\n';
        std::abort();
    }

    g_phase.store(3, std::memory_order_release);
    SyntheticFn function = target.function();
    for (int value = 0; value < 32; ++value) {
        assert(function(value) == value + 1);
    }

    g_phase.store(4, std::memory_order_release);
    if (!hook.restore(error)) {
        std::cerr << "retired-fault-probe restore failed cycle=" << cycle << " error=" << error << '\n';
        std::abort();
    }

    g_phase.store(5, std::memory_order_release);
    if (!hook.proveQuiescence(g_active, 5000, error)) {
        std::cerr << "retired-fault-probe quiescence failed cycle=" << cycle << " error=" << error << '\n';
        std::abort();
    }

    g_phase.store(6, std::memory_order_release);
    if (!hook.destroy(error)) {
        std::cerr << "retired-fault-probe destroy failed cycle=" << cycle << " error=" << error << '\n';
        std::abort();
    }
    g_relay.store(nullptr, std::memory_order_release);
    g_trampoline.store(nullptr, std::memory_order_release);
    g_phase.store(7, std::memory_order_release);
    assert(function(9) == 10);
}

}  // namespace

int main()
{
    std::cerr << "stage=retired-fault-probe begin\n";
    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    assert(function(41) == 42);

    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = ::SetUnhandledExceptionFilter(&stressUnhandledExceptionFilter);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> worker_calls{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = static_cast<int>(worker + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const int result = function(value);
                if (result != value + 1) {
                    std::fprintf(stderr,
                                 "stage=retired-fault-probe mismatch worker=%llu generation=%llu cycle=%llu phase=%u "
                                 "value=%d result=%d active=%llu relay=%p trampoline=%p\n",
                                 static_cast<unsigned long long>(worker),
                                 static_cast<unsigned long long>(g_generation.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
                                 g_phase.load(std::memory_order_relaxed), value, result,
                                 static_cast<unsigned long long>(g_active.load(std::memory_order_relaxed)),
                                 g_relay.load(std::memory_order_relaxed), g_trampoline.load(std::memory_order_relaxed));
                    std::fflush(stderr);
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        runCycle(target, cycle);
        if ((cycle + 1) % 25 == 0) {
            std::cerr << "stage=retired-fault-probe progress=" << (cycle + 1) << '/' << kCycles << '\n';
        }
    }

    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }
    (void)::SetUnhandledExceptionFilter(previous_filter);

    assert(g_hook_calls.load(std::memory_order_relaxed) != 0);
    assert(g_stale_calls.load(std::memory_order_relaxed) == 0);
    assert(worker_calls.load(std::memory_order_relaxed) != 0);
    std::cerr << "stage=retired-fault-probe pass cycles=" << kCycles
              << " worker_calls=" << worker_calls.load(std::memory_order_relaxed)
              << " hook_calls=" << g_hook_calls.load(std::memory_order_relaxed) << '\n';
    return 0;
}
