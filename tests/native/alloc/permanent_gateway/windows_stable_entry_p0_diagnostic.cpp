#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_stable_entry_p0_diagnostic.cpp is Windows-only"
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::AtomicEntryHook;

namespace {

using SyntheticFn = int(__cdecl *)(int);

constexpr std::size_t kTargetReservationSize = 64 * 1024;
constexpr std::size_t kMaxGenerations = 4096;
constexpr std::size_t kCycles = 1500;
constexpr std::size_t kWorkers = 8;

struct GenerationRecord {
    std::atomic<std::uint32_t> state{0};  // 0 empty, 1 live, 2 reclaimed
    std::uint64_t generation = 0;
    std::uintptr_t relay_region_begin = 0;
    std::uintptr_t relay_region_end = 0;
    std::uintptr_t relay_code_begin = 0;
    std::uintptr_t relay_code_end = 0;
    std::uintptr_t trampoline_region_begin = 0;
    std::uintptr_t trampoline_region_end = 0;
    std::uintptr_t trampoline_entry = 0;
};

std::array<GenerationRecord, kMaxGenerations> g_generations{};
std::atomic<std::uint64_t> g_stress_cycle{0};
std::atomic<unsigned> g_stress_phase{0};
std::atomic<void *> g_synthetic_trampoline{nullptr};
std::atomic<void *> g_current_relay{nullptr};
std::atomic<std::uint64_t> g_synthetic_active{0};
std::atomic<std::uint64_t> g_synthetic_hook_calls{0};
std::atomic<std::uint64_t> g_synthetic_stale_calls{0};
std::atomic<std::uintptr_t> g_target_begin{0};
std::atomic<std::uintptr_t> g_target_end{0};
std::atomic<std::uintptr_t> g_hook_begin{0};
std::atomic<std::uintptr_t> g_hook_end{0};

[[nodiscard]] bool contains(std::uintptr_t address, std::uintptr_t begin, std::uintptr_t end) noexcept
{
    return begin != 0 && begin <= address && address < end;
}

[[nodiscard]] std::uint64_t hashEntry8() noexcept
{
    const std::uintptr_t begin = g_target_begin.load(std::memory_order_relaxed);
    if (begin == 0) {
        return 0;
    }
    std::array<std::uint8_t, 8> bytes{};
    std::memcpy(bytes.data(), reinterpret_cast<const void *>(begin), bytes.size());
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void publishGeneration(const AtomicEntryHook &hook, std::size_t cycle) noexcept
{
    assert(cycle < g_generations.size());
    GenerationRecord &record = g_generations[cycle];
    assert(record.state.load(std::memory_order_relaxed) == 0);

    MEMORY_BASIC_INFORMATION relay_memory{};
    MEMORY_BASIC_INFORMATION trampoline_memory{};
    assert(::VirtualQuery(hook.relay(), &relay_memory, sizeof(relay_memory)) != 0);
    assert(::VirtualQuery(hook.trampoline(), &trampoline_memory, sizeof(trampoline_memory)) != 0);

    record.generation = cycle;
    record.relay_region_begin = reinterpret_cast<std::uintptr_t>(relay_memory.BaseAddress);
    record.relay_region_end = record.relay_region_begin + relay_memory.RegionSize;
    record.relay_code_begin = reinterpret_cast<std::uintptr_t>(hook.relay());
    record.relay_code_end = record.relay_code_begin + spark::stable_entry_experiment::kAbsoluteIndirectJumpSize;
    record.trampoline_region_begin = reinterpret_cast<std::uintptr_t>(trampoline_memory.BaseAddress);
    record.trampoline_region_end = record.trampoline_region_begin + trampoline_memory.RegionSize;
    record.trampoline_entry = reinterpret_cast<std::uintptr_t>(hook.trampoline());
    record.state.store(1, std::memory_order_release);
}

struct HistoricalMatch {
    bool found = false;
    std::uint64_t generation = 0;
    std::uint32_t state = 0;
    const char *kind = "none";
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

[[nodiscard]] HistoricalMatch classifyHistorical(std::uintptr_t rip) noexcept
{
    HistoricalMatch match;
    const std::uint64_t current = g_stress_cycle.load(std::memory_order_relaxed);
    const std::size_t limit = static_cast<std::size_t>((current + 1 < g_generations.size()) ? current + 1
                                                                                           : g_generations.size());
    for (std::size_t index = 0; index < limit; ++index) {
        const GenerationRecord &record = g_generations[index];
        const std::uint32_t state = record.state.load(std::memory_order_acquire);
        if (state == 0) {
            continue;
        }
        const char *kind = nullptr;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        if (contains(rip, record.trampoline_region_begin, record.trampoline_region_end)) {
            kind = "trampoline-region";
            begin = record.trampoline_region_begin;
            end = record.trampoline_region_end;
        }
        else if (contains(rip, record.relay_code_begin, record.relay_code_end)) {
            kind = "relay-code";
            begin = record.relay_code_begin;
            end = record.relay_code_end;
        }
        else if (contains(rip, record.relay_region_begin, record.relay_region_end)) {
            kind = "relay-region";
            begin = record.relay_region_begin;
            end = record.relay_region_end;
        }
        if (kind != nullptr && (!match.found || record.generation >= match.generation)) {
            match.found = true;
            match.generation = record.generation;
            match.state = state;
            match.kind = kind;
            match.begin = begin;
            match.end = end;
        }
    }
    return match;
}

LONG WINAPI diagnosticUnhandledExceptionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    const EXCEPTION_RECORD *record = exception != nullptr ? exception->ExceptionRecord : nullptr;
    const CONTEXT *context = exception != nullptr ? exception->ContextRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    const void *exception_address = record != nullptr ? record->ExceptionAddress : nullptr;
    std::uintptr_t rip = 0;
#if defined(_M_X64)
    if (context != nullptr) {
        rip = static_cast<std::uintptr_t>(context->Rip);
    }
#endif

    ULONG_PTR operation = static_cast<ULONG_PTR>(-1);
    ULONG_PTR fault_address = 0;
    if (record != nullptr && code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        operation = record->ExceptionInformation[0];
        fault_address = record->ExceptionInformation[1];
    }

    const HistoricalMatch historical = classifyHistorical(rip);
    const std::uintptr_t target_begin = g_target_begin.load(std::memory_order_relaxed);
    const std::uintptr_t target_end = g_target_end.load(std::memory_order_relaxed);
    const std::uintptr_t hook_begin = g_hook_begin.load(std::memory_order_relaxed);
    const std::uintptr_t hook_end = g_hook_end.load(std::memory_order_relaxed);
    const char *coarse = contains(rip, target_begin, target_end) ? "target-allocation"
                         : contains(rip, hook_begin, hook_end)   ? "spark-hook"
                         : historical.found                    ? historical.kind
                                                                : "other";

    MEMORY_BASIC_INFORMATION memory{};
    const SIZE_T queried = rip != 0 ? ::VirtualQuery(reinterpret_cast<const void *>(rip), &memory, sizeof(memory)) : 0;

    std::fprintf(
        stderr,
        "p0-fault code=0x%08lx exception_address=%p rip=0x%llx av_operation=%llu av_fault=0x%llx "
        "cycle=%llu phase=%u active=%llu current_trampoline=%p current_relay=%p coarse=%s "
        "historical_found=%u historical_generation=%llu historical_state=%u historical_kind=%s "
        "historical_begin=0x%llx historical_end=0x%llx entry_hash8=0x%016llx "
        "vq=%llu vq_base=%p vq_allocbase=%p vq_regionsize=%llu vq_state=0x%lx vq_protect=0x%lx vq_type=0x%lx\n",
        static_cast<unsigned long>(code), exception_address, static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(operation), static_cast<unsigned long long>(fault_address),
        static_cast<unsigned long long>(g_stress_cycle.load(std::memory_order_relaxed)),
        g_stress_phase.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_synthetic_active.load(std::memory_order_relaxed)),
        g_synthetic_trampoline.load(std::memory_order_relaxed), g_current_relay.load(std::memory_order_relaxed), coarse,
        historical.found ? 1U : 0U, static_cast<unsigned long long>(historical.generation), historical.state,
        historical.kind, static_cast<unsigned long long>(historical.begin), static_cast<unsigned long long>(historical.end),
        static_cast<unsigned long long>(hashEntry8()), static_cast<unsigned long long>(queried), memory.BaseAddress,
        memory.AllocationBase, static_cast<unsigned long long>(memory.RegionSize), static_cast<unsigned long>(memory.State),
        static_cast<unsigned long>(memory.Protect), static_cast<unsigned long>(memory.Type));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

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

class ExecutableSyntheticFunction {
public:
    ExecutableSyntheticFunction()
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, kTargetReservationSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(page_ != nullptr);
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(page_, code.data(), code.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, code.size()) != FALSE);
        DWORD old = 0;
        assert(::VirtualProtect(page_, kTargetReservationSize, PAGE_EXECUTE_READ, &old) != FALSE);
        g_target_begin.store(reinterpret_cast<std::uintptr_t>(page_), std::memory_order_release);
        g_target_end.store(reinterpret_cast<std::uintptr_t>(page_) + kTargetReservationSize, std::memory_order_release);
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

void initializeHookRange() noexcept
{
#if defined(_M_X64)
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function =
        ::RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(&syntheticHook), &image_base, nullptr);
    assert(function != nullptr);
    g_hook_begin.store(static_cast<std::uintptr_t>(image_base + function->BeginAddress), std::memory_order_release);
    g_hook_end.store(static_cast<std::uintptr_t>(image_base + function->EndAddress), std::memory_order_release);
#endif
}

void runSyntheticCycle(ExecutableSyntheticFunction &target, std::size_t cycle)
{
    SyntheticFn function = target.function();
    AtomicEntryHook hook;
    std::string error;

    g_stress_cycle.store(cycle, std::memory_order_release);
    g_stress_phase.store(1, std::memory_order_release);
    if (!hook.prepare(target.address(), reinterpret_cast<void *>(&syntheticHook), error)) {
        std::fprintf(stderr, "p0-prepare-failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                     error.c_str());
        std::abort();
    }
    g_synthetic_trampoline.store(hook.trampoline(), std::memory_order_release);
    g_current_relay.store(hook.relay(), std::memory_order_release);
    publishGeneration(hook, cycle);

    g_stress_phase.store(2, std::memory_order_release);
    if (!hook.install(error)) {
        std::fprintf(stderr, "p0-install-failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                     error.c_str());
        std::abort();
    }

    g_stress_phase.store(3, std::memory_order_release);
    for (int i = 0; i < 32; ++i) {
        assert(function(i) == i + 1);
    }

    g_stress_phase.store(4, std::memory_order_release);
    if (!hook.restore(error)) {
        std::fprintf(stderr, "p0-restore-failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                     error.c_str());
        std::abort();
    }

    g_stress_phase.store(5, std::memory_order_release);
    if (!hook.proveQuiescence(g_synthetic_active, 5000, error)) {
        std::fprintf(stderr, "p0-quiescence-failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                     error.c_str());
        std::abort();
    }

    g_stress_phase.store(6, std::memory_order_release);
    if (!hook.destroy(error)) {
        std::fprintf(stderr, "p0-destroy-failed cycle=%llu error=%s\n", static_cast<unsigned long long>(cycle),
                     error.c_str());
        std::abort();
    }
    g_generations[cycle].state.store(2, std::memory_order_release);
    g_synthetic_trampoline.store(nullptr, std::memory_order_release);
    g_current_relay.store(nullptr, std::memory_order_release);

    g_stress_phase.store(7, std::memory_order_release);
    assert(function(9) == 10);
}

}  // namespace

int main()
{
    std::fprintf(stderr, "p0-diagnostic begin cycles=%llu workers=%llu\n", static_cast<unsigned long long>(kCycles),
                 static_cast<unsigned long long>(kWorkers));
    initializeHookRange();
    LPTOP_LEVEL_EXCEPTION_FILTER previous = ::SetUnhandledExceptionFilter(&diagnosticUnhandledExceptionFilter);

    ExecutableSyntheticFunction target;
    SyntheticFn function = target.function();
    assert(function(41) == 42);

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
                                 "p0-mismatch worker=%llu cycle=%llu phase=%u value=%d result=%d active=%llu\n",
                                 static_cast<unsigned long long>(worker),
                                 static_cast<unsigned long long>(g_stress_cycle.load(std::memory_order_relaxed)),
                                 g_stress_phase.load(std::memory_order_relaxed), value, result,
                                 static_cast<unsigned long long>(g_synthetic_active.load(std::memory_order_relaxed)));
                    std::fflush(stderr);
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
        runSyntheticCycle(target, cycle);
        if ((cycle + 1) % 25 == 0) {
            std::fprintf(stderr, "p0-progress=%llu/%llu\n", static_cast<unsigned long long>(cycle + 1),
                         static_cast<unsigned long long>(kCycles));
        }
    }

    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }
    (void)::SetUnhandledExceptionFilter(previous);

    std::fprintf(stderr, "p0-diagnostic pass cycles=%llu worker_calls=%llu hook_calls=%llu stale=%llu\n",
                 static_cast<unsigned long long>(kCycles),
                 static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_synthetic_hook_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_synthetic_stale_calls.load(std::memory_order_relaxed)));
    return g_synthetic_stale_calls.load(std::memory_order_relaxed) == 0 ? 0 : 2;
}
