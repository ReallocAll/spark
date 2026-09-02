#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_stable_entry_p0_context_diagnostic.cpp is Windows-only"
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
constexpr std::size_t kReservation = 64 * 1024;
constexpr std::size_t kCycles = 1800;
constexpr std::size_t kWorkers = 8;

std::atomic<std::uintptr_t> g_target{0};
std::atomic<void *> g_trampoline{nullptr};
std::atomic<void *> g_relay{nullptr};
std::atomic<std::uint64_t> g_active{0};
std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_cycle{0};
std::atomic<unsigned> g_phase{0};
std::atomic<std::uint64_t> g_original8{0};
std::atomic<std::uint64_t> g_installed8{0};

[[nodiscard]] std::uint64_t first8(const std::array<std::uint8_t, 16> &bytes) noexcept
{
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

void queryMemory(std::uintptr_t address, const char *label) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    const SIZE_T result =
        address != 0 ? ::VirtualQuery(reinterpret_cast<const void *>(address), &memory, sizeof(memory)) : 0;
    std::fprintf(stderr,
                 " %s_vq=%llu %s_base=%p %s_allocbase=%p %s_size=%llu %s_state=0x%lx %s_protect=0x%lx %s_type=0x%lx",
                 label, static_cast<unsigned long long>(result), label, memory.BaseAddress, label, memory.AllocationBase,
                 label, static_cast<unsigned long long>(memory.RegionSize), label,
                 static_cast<unsigned long>(memory.State), label, static_cast<unsigned long>(memory.Protect), label,
                 static_cast<unsigned long>(memory.Type));
}

LONG WINAPI diagnosticFilter(EXCEPTION_POINTERS *exception) noexcept
{
    const EXCEPTION_RECORD *record = exception != nullptr ? exception->ExceptionRecord : nullptr;
    const CONTEXT *context = exception != nullptr ? exception->ContextRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    const std::uintptr_t target = g_target.load(std::memory_order_relaxed);
    const std::uintptr_t rip = context != nullptr ? static_cast<std::uintptr_t>(context->Rip) : 0;
    const std::int64_t target_offset = target != 0 ? static_cast<std::int64_t>(rip - target) : INT64_MIN;

    ULONG_PTR operation = static_cast<ULONG_PTR>(-1);
    ULONG_PTR fault = 0;
    if (record != nullptr && code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        operation = record->ExceptionInformation[0];
        fault = record->ExceptionInformation[1];
    }

    std::array<std::uint8_t, 16> entry{};
    SIZE_T entry_read = 0;
    if (target != 0) {
        (void)::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void *>(target), entry.data(),
                                  entry.size(), &entry_read);
    }
    std::uint64_t current_low = 0;
    std::uint64_t current_high = 0;
    std::memcpy(&current_low, entry.data(), sizeof(current_low));
    std::memcpy(&current_high, entry.data() + 8, sizeof(current_high));

    std::fprintf(stderr,
                 "p0-context-fault code=0x%08lx exception=%p rip=0x%llx target=0x%llx target_offset=%lld "
                 "operation=%llu fault=0x%llx cycle=%llu phase=%u active=%llu trampoline=%p relay=%p "
                 "entry_read=%llu entry_low=0x%016llx entry_high=0x%016llx original8=0x%016llx installed8=0x%016llx",
                 static_cast<unsigned long>(code), record != nullptr ? record->ExceptionAddress : nullptr,
                 static_cast<unsigned long long>(rip), static_cast<unsigned long long>(target),
                 static_cast<long long>(target_offset), static_cast<unsigned long long>(operation),
                 static_cast<unsigned long long>(fault),
                 static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
                 g_phase.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(g_active.load(std::memory_order_relaxed)),
                 g_trampoline.load(std::memory_order_relaxed), g_relay.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(entry_read), static_cast<unsigned long long>(current_low),
                 static_cast<unsigned long long>(current_high),
                 static_cast<unsigned long long>(g_original8.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_installed8.load(std::memory_order_relaxed)));

    if (context != nullptr) {
        std::fprintf(stderr,
                     " rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx "
                     "r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx "
                     "r15=0x%llx rsp=0x%llx rbp=0x%llx eflags=0x%lx",
                     static_cast<unsigned long long>(context->Rax), static_cast<unsigned long long>(context->Rbx),
                     static_cast<unsigned long long>(context->Rcx), static_cast<unsigned long long>(context->Rdx),
                     static_cast<unsigned long long>(context->Rsi), static_cast<unsigned long long>(context->Rdi),
                     static_cast<unsigned long long>(context->R8), static_cast<unsigned long long>(context->R9),
                     static_cast<unsigned long long>(context->R10), static_cast<unsigned long long>(context->R11),
                     static_cast<unsigned long long>(context->R12), static_cast<unsigned long long>(context->R13),
                     static_cast<unsigned long long>(context->R14), static_cast<unsigned long long>(context->R15),
                     static_cast<unsigned long long>(context->Rsp), static_cast<unsigned long long>(context->Rbp),
                     static_cast<unsigned long>(context->EFlags));
    }
    queryMemory(rip, "rip");
    queryMemory(static_cast<std::uintptr_t>(fault), "fault");
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(noinline) int __cdecl syntheticHook(int value) noexcept
{
    g_active.fetch_add(1, std::memory_order_acq_rel);
    auto *callable = reinterpret_cast<SyntheticFn>(g_trampoline.load(std::memory_order_acquire));
    if (callable == nullptr) {
        g_active.fetch_sub(1, std::memory_order_release);
        return value + 1;
    }
    const int result = callable(value);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_active.fetch_sub(1, std::memory_order_release);
    return result;
}

class Target {
public:
    Target()
    {
        memory_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, kReservation, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(memory_ != nullptr);
        constexpr std::array<std::uint8_t, 16> bytes{
            0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(memory_, bytes.data(), bytes.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), memory_, bytes.size()) != FALSE);
        DWORD old = 0;
        assert(::VirtualProtect(memory_, kReservation, PAGE_EXECUTE_READ, &old) != FALSE);
        g_target.store(reinterpret_cast<std::uintptr_t>(memory_), std::memory_order_release);
    }

    ~Target()
    {
        if (memory_ != nullptr) {
            ::VirtualFree(memory_, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] void *address() const noexcept { return memory_; }
    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(memory_); }

private:
    std::uint8_t *memory_ = nullptr;
};

void cycle(Target &target, std::size_t index)
{
    AtomicEntryHook hook;
    std::string error;
    SyntheticFn function = target.function();
    g_cycle.store(index, std::memory_order_release);
    g_phase.store(1, std::memory_order_release);
    if (!hook.prepare(target.address(), reinterpret_cast<void *>(&syntheticHook), error)) {
        std::fprintf(stderr, "p0-context prepare failure cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(index), error.c_str());
        std::abort();
    }
    g_original8.store(first8(hook.originalBytes()), std::memory_order_release);
    g_installed8.store(first8(hook.installedBytes()), std::memory_order_release);
    g_trampoline.store(hook.trampoline(), std::memory_order_release);
    g_relay.store(hook.relay(), std::memory_order_release);

    g_phase.store(2, std::memory_order_release);
    if (!hook.install(error)) {
        std::fprintf(stderr, "p0-context install failure cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(index), error.c_str());
        std::abort();
    }
    g_phase.store(3, std::memory_order_release);
    for (int value = 0; value != 32; ++value) {
        assert(function(value) == value + 1);
    }
    g_phase.store(4, std::memory_order_release);
    if (!hook.restore(error)) {
        std::fprintf(stderr, "p0-context restore failure cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(index), error.c_str());
        std::abort();
    }
    g_phase.store(5, std::memory_order_release);
    if (!hook.proveQuiescence(g_active, 5000, error)) {
        std::fprintf(stderr, "p0-context quiescence failure cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(index), error.c_str());
        std::abort();
    }
    g_phase.store(6, std::memory_order_release);
    if (!hook.destroy(error)) {
        std::fprintf(stderr, "p0-context destroy failure cycle=%llu error=%s\n",
                     static_cast<unsigned long long>(index), error.c_str());
        std::abort();
    }
    g_trampoline.store(nullptr, std::memory_order_release);
    g_relay.store(nullptr, std::memory_order_release);
    g_phase.store(7, std::memory_order_release);
    assert(function(9) == 10);
}

}  // namespace

int main()
{
    std::fprintf(stderr, "p0-context begin cycles=%llu workers=%llu\n", static_cast<unsigned long long>(kCycles),
                 static_cast<unsigned long long>(kWorkers));
    LPTOP_LEVEL_EXCEPTION_FILTER previous = ::SetUnhandledExceptionFilter(&diagnosticFilter);
    Target target;
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
                    std::fprintf(stderr, "p0-context mismatch worker=%llu cycle=%llu phase=%u value=%d result=%d\n",
                                 static_cast<unsigned long long>(worker),
                                 static_cast<unsigned long long>(g_cycle.load(std::memory_order_relaxed)),
                                 g_phase.load(std::memory_order_relaxed), value, result);
                    std::abort();
                }
                value = (value + 1) & 0x7fff;
                worker_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t index = 0; index < kCycles; ++index) {
        cycle(target, index);
        if ((index + 1) % 25 == 0) {
            std::fprintf(stderr, "p0-context progress=%llu/%llu\n", static_cast<unsigned long long>(index + 1),
                         static_cast<unsigned long long>(kCycles));
        }
    }
    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }
    (void)::SetUnhandledExceptionFilter(previous);
    std::fprintf(stderr, "p0-context pass cycles=%llu worker_calls=%llu hook_calls=%llu\n",
                 static_cast<unsigned long long>(kCycles),
                 static_cast<unsigned long long>(worker_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_calls.load(std::memory_order_relaxed)));
    return 0;
}
