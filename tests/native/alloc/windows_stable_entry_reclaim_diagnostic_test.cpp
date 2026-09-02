#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_stable_entry_reclaim_diagnostic_test.cpp is Windows-only"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::AddressRange;
using spark::stable_entry_experiment::AtomicEntryHook;

namespace {

using SyntheticFn = int(__cdecl *)(int);

struct GenerationRanges {
    std::uint64_t generation = 0;
    AddressRange relay{};
    AddressRange relay_allocation{};
    AddressRange trampoline{};
    AddressRange trampoline_allocation{};
    AddressRange hook{};
};

constexpr std::size_t kMaxRecordedGenerations = 4096;
std::array<GenerationRanges, kMaxRecordedGenerations> g_history{};
std::atomic<std::size_t> g_history_count{0};
std::atomic<void *> g_trampoline{nullptr};
std::atomic<void *> g_relay{nullptr};
std::atomic<void *> g_entry{nullptr};
std::atomic<std::uint64_t> g_active{0};
std::atomic<std::uint64_t> g_hook_calls{0};
std::atomic<std::uint64_t> g_cycle{0};
std::atomic<unsigned> g_phase{0};

AddressRange allocationRangeFor(void *address) noexcept
{
    if (address == nullptr) {
        return {};
    }
    MEMORY_BASIC_INFORMATION first{};
    if (::VirtualQuery(address, &first, sizeof(first)) == 0 || first.AllocationBase == nullptr) {
        return {};
    }

    const auto allocation_base = reinterpret_cast<std::uintptr_t>(first.AllocationBase);
    std::uintptr_t cursor = allocation_base;
    std::uintptr_t end = allocation_base;
    for (std::size_t region = 0; region < 4096; ++region) {
        MEMORY_BASIC_INFORMATION memory{};
        if (::VirtualQuery(reinterpret_cast<const void *>(cursor), &memory, sizeof(memory)) == 0 ||
            memory.AllocationBase != first.AllocationBase || memory.RegionSize == 0) {
            break;
        }
        const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto region_end = region_begin + memory.RegionSize;
        if (region_end <= cursor || region_end < region_begin) {
            break;
        }
        end = region_end;
        cursor = region_end;
    }
    return {allocation_base, end};
}

const char *rangeClass(std::uintptr_t rip, const GenerationRanges &record) noexcept
{
    if (record.relay.contains(rip)) {
        return "relay-code";
    }
    if (record.trampoline.contains(rip)) {
        return "trampoline-region";
    }
    if (record.hook.contains(rip)) {
        return "hook";
    }
    if (record.relay_allocation.contains(rip)) {
        return "relay-allocation";
    }
    if (record.trampoline_allocation.contains(rip)) {
        return "trampoline-allocation";
    }
    return nullptr;
}

LONG WINAPI diagnosticUnhandledExceptionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    DWORD code = 0;
    const void *exception_address = nullptr;
    std::uintptr_t rip = 0;
    ULONG_PTR access_kind = static_cast<ULONG_PTR>(-1);
    ULONG_PTR access_address = 0;
    if (exception != nullptr && exception->ExceptionRecord != nullptr) {
        code = exception->ExceptionRecord->ExceptionCode;
        exception_address = exception->ExceptionRecord->ExceptionAddress;
        if (exception->ExceptionRecord->NumberParameters >= 2) {
            access_kind = exception->ExceptionRecord->ExceptionInformation[0];
            access_address = exception->ExceptionRecord->ExceptionInformation[1];
        }
    }
#if defined(_M_X64)
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        rip = static_cast<std::uintptr_t>(exception->ContextRecord->Rip);
    }
#endif

    const std::size_t count = g_history_count.load(std::memory_order_acquire);
    const char *classification = "unclassified";
    std::uint64_t classified_generation = 0;
    for (std::size_t i = count; i != 0; --i) {
        const GenerationRanges &record = g_history[i - 1];
        if (const char *candidate = rangeClass(rip, record); candidate != nullptr) {
            classification = candidate;
            classified_generation = record.generation;
            break;
        }
    }

    MEMORY_BASIC_INFORMATION memory{};
    const SIZE_T queried = ::VirtualQuery(reinterpret_cast<const void *>(rip), &memory, sizeof(memory));
    std::fprintf(stderr,
                 "stage=reclaim-diagnostic exception code=0x%08lx exception_address=%p rip=0x%llx "
                 "access_kind=%llu access_address=0x%llx cycle=%llu phase=%u active=%llu entry=%p relay=%p "
                 "trampoline=%p class=%s class_generation=%llu history=%llu vq=%llu base=%p alloc_base=%p "
                 "region=%llu state=0x%08lx protect=0x%08lx type=0x%08lx\n",
                 static_cast<unsigned long>(code), exception_address, static_cast<unsigned long long>(rip),
                 static_cast<unsigned long long>(access_kind), static_cast<unsigned long long>(access_address),
                 static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
                 g_phase.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_active.load(std::memory_order_relaxed)),
                 g_entry.load(std::memory_order_relaxed), g_relay.load(std::memory_order_relaxed),
                 g_trampoline.load(std::memory_order_relaxed), classification,
                 static_cast<unsigned long long>(classified_generation), static_cast<unsigned long long>(count),
                 static_cast<unsigned long long>(queried), memory.BaseAddress, memory.AllocationBase,
                 static_cast<unsigned long long>(memory.RegionSize), static_cast<unsigned long>(memory.State),
                 static_cast<unsigned long>(memory.Protect), static_cast<unsigned long>(memory.Type));

    if (classified_generation != 0 && classified_generation <= count) {
        const GenerationRanges &record = g_history[static_cast<std::size_t>(classified_generation - 1)];
        std::fprintf(stderr,
                     "stage=reclaim-diagnostic classified generation=%llu relay=[0x%llx,0x%llx) "
                     "relay_allocation=[0x%llx,0x%llx) trampoline=[0x%llx,0x%llx) "
                     "trampoline_allocation=[0x%llx,0x%llx) hook=[0x%llx,0x%llx)\n",
                     static_cast<unsigned long long>(record.generation),
                     static_cast<unsigned long long>(record.relay.begin),
                     static_cast<unsigned long long>(record.relay.end),
                     static_cast<unsigned long long>(record.relay_allocation.begin),
                     static_cast<unsigned long long>(record.relay_allocation.end),
                     static_cast<unsigned long long>(record.trampoline.begin),
                     static_cast<unsigned long long>(record.trampoline.end),
                     static_cast<unsigned long long>(record.trampoline_allocation.begin),
                     static_cast<unsigned long long>(record.trampoline_allocation.end),
                     static_cast<unsigned long long>(record.hook.begin),
                     static_cast<unsigned long long>(record.hook.end));
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(noinline) int __cdecl diagnosticHook(int value) noexcept
{
    g_active.fetch_add(1, std::memory_order_acq_rel);
    auto *callable = reinterpret_cast<SyntheticFn>(g_trampoline.load(std::memory_order_acquire));
    if (callable == nullptr) {
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
        assert((reinterpret_cast<std::uintptr_t>(page_) & 7U) == 0);
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
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

void recordGeneration(const AtomicEntryHook &hook, std::size_t cycle)
{
    const auto ranges = hook.protectedRanges();
    assert(ranges.size() == 3);
    assert(cycle < g_history.size());
    GenerationRanges record;
    record.generation = static_cast<std::uint64_t>(cycle + 1);
    record.relay = ranges[0];
    record.relay_allocation = allocationRangeFor(hook.relay());
    record.trampoline = ranges[1];
    record.trampoline_allocation = allocationRangeFor(hook.trampoline());
    record.hook = ranges[2];
    assert(record.relay_allocation.contains(reinterpret_cast<std::uintptr_t>(hook.relay())));
    assert(record.trampoline_allocation.contains(reinterpret_cast<std::uintptr_t>(hook.trampoline())));
    g_history[cycle] = record;
    g_history_count.store(cycle + 1, std::memory_order_release);
}

void runCycle(ExecutableSyntheticFunction &target, std::size_t cycle)
{
    AtomicEntryHook hook;
    std::string error;
    g_cycle.store(cycle + 1, std::memory_order_release);
    g_phase.store(1, std::memory_order_release);
    if (!hook.prepare(target.address(), reinterpret_cast<void *>(&diagnosticHook), error)) {
        std::fprintf(stderr, "stage=reclaim-diagnostic prepare-failed cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(cycle + 1), error.c_str());
        std::abort();
    }
    recordGeneration(hook, cycle);
    g_entry.store(target.address(), std::memory_order_release);
    g_relay.store(hook.relay(), std::memory_order_release);
    g_trampoline.store(hook.trampoline(), std::memory_order_release);

    g_phase.store(2, std::memory_order_release);
    if (!hook.install(error)) {
        std::fprintf(stderr, "stage=reclaim-diagnostic install-failed cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(cycle + 1), error.c_str());
        std::abort();
    }

    g_phase.store(3, std::memory_order_release);
    g_phase.store(4, std::memory_order_release);
    if (!hook.restore(error)) {
        std::fprintf(stderr, "stage=reclaim-diagnostic restore-failed cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(cycle + 1), error.c_str());
        std::abort();
    }

    g_phase.store(5, std::memory_order_release);
    if (!hook.proveQuiescence(g_active, 5000, error)) {
        std::fprintf(stderr, "stage=reclaim-diagnostic quiescence-failed cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(cycle + 1), error.c_str());
        std::abort();
    }

    g_phase.store(6, std::memory_order_release);
    if (!hook.destroy(error)) {
        std::fprintf(stderr, "stage=reclaim-diagnostic destroy-failed cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(cycle + 1), error.c_str());
        std::abort();
    }
    g_trampoline.store(nullptr, std::memory_order_release);
    g_relay.store(nullptr, std::memory_order_release);
    g_phase.store(7, std::memory_order_release);
}

}  // namespace

int main()
{
    std::fprintf(stderr, "stage=reclaim-diagnostic begin\n");
    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    assert(function(41) == 42);

    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = ::SetUnhandledExceptionFilter(&diagnosticUnhandledExceptionFilter);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> worker_calls{0};
    constexpr std::size_t kWorkers = 8;
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = static_cast<int>(worker + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const int result = function(value);
                if (result != value + 1) {
                    std::fprintf(stderr,
                                 "stage=reclaim-diagnostic mismatch worker=%llu cycle=%llu phase=%u value=%d result=%d "
                                 "active=%llu relay=%p trampoline=%p\n",
                                 static_cast<unsigned long long>(worker),
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

    constexpr std::size_t kCycles = 3000;
    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        runCycle(target, cycle);
        if ((cycle + 1) % 25 == 0) {
            std::fprintf(stderr, "stage=reclaim-diagnostic progress=%llu/%llu calls=%llu\n",
                         static_cast<unsigned long long>(cycle + 1), static_cast<unsigned long long>(kCycles),
                         static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)));
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }
    (void)::SetUnhandledExceptionFilter(previous_filter);
    assert(worker_calls.load(std::memory_order_relaxed) != 0);
    assert(g_hook_calls.load(std::memory_order_relaxed) != 0);
    std::fprintf(stderr, "stage=reclaim-diagnostic pass cycles=%llu calls=%llu hook_calls=%llu\n",
                 static_cast<unsigned long long>(kCycles),
                 static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_hook_calls.load(std::memory_order_relaxed)));
    return 0;
}
