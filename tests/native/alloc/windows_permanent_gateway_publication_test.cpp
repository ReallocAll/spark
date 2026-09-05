#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_publication_test.cpp is Windows-only"
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using spark::stable_entry_experiment::discoverPermanentGateway;
using spark::stable_entry_experiment::installPermanentGateway;
using spark::stable_entry_experiment::PermanentGatewayHandle;

namespace {

using SyntheticFn = int(__cdecl *)(int);

constexpr std::size_t kPublicationRounds = 64;
constexpr std::size_t kWorkers = 8;
constexpr std::size_t kChurners = 2;
constexpr std::uint64_t kPrePublicationCalls = 20000;
constexpr std::uint64_t kPostPublicationCalls = 20000;
constexpr std::uint64_t kChurnBatchCalls = 128;
constexpr std::uint64_t kCounterTimeoutMs = 5000;
constexpr std::size_t kNoRound = (std::numeric_limits<std::size_t>::max)();

std::atomic<std::size_t> g_round{kNoRound};
std::atomic<unsigned> g_phase{0};
std::atomic<void *> g_entry{nullptr};
std::atomic<void *> g_gateway{nullptr};

[[noreturn]] void failInvariant(const char *invariant, std::size_t round, const void *entry = nullptr,
                                const void *gateway = nullptr)
{
    std::fprintf(stderr,
                 "stage=permanent-gateway-publication invariant-failure invariant=%s round=%zu phase=%u "
                 "entry=%p gateway=%p\n",
                 invariant, round, g_phase.load(std::memory_order_relaxed), entry, gateway);
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] void failCallResult(const char *caller, std::size_t round, std::size_t worker, int input, int actual,
                                 const void *entry)
{
    std::fprintf(stderr,
                 "stage=permanent-gateway-publication call-mismatch caller=%s round=%zu phase=%u worker=%zu "
                 "input=%d actual=%d expected=%d entry=%p gateway=%p\n",
                 caller, round, g_phase.load(std::memory_order_relaxed), worker, input, actual, input + 1, entry,
                 g_gateway.load(std::memory_order_relaxed));
    std::fflush(stderr);
    std::abort();
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

LONG WINAPI publicationUnhandledExceptionFilter(EXCEPTION_POINTERS *exception) noexcept
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

    std::fprintf(stderr,
                 "stage=permanent-gateway-publication exception code=0x%08lx exception_address=%p rip=0x%llx "
                 "av_op=%llu av_kind=%s av_target=0x%llx round=%zu phase=%u entry=%p gateway=%p\n",
                 static_cast<unsigned long>(code), exception_address, static_cast<unsigned long long>(rip),
                 static_cast<unsigned long long>(av_operation), accessKind(av_operation),
                 static_cast<unsigned long long>(av_target), g_round.load(std::memory_order_relaxed),
                 g_phase.load(std::memory_order_relaxed), g_entry.load(std::memory_order_relaxed),
                 g_gateway.load(std::memory_order_relaxed));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

[[nodiscard]] bool isExecutable(DWORD protection) noexcept
{
    const DWORD base = protection & 0xFFU;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] void *allocateProcessLifetimeTarget(std::size_t round)
{
    constexpr std::array<std::uint8_t, 16> code{0x8D, 0x41, 0x01, 0x66, 0x90, 0xC3, 0x90, 0x90,
                                                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

    auto *page =
        static_cast<std::uint8_t *>(::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (page == nullptr) {
        failInvariant("target-virtualalloc", round);
    }
    if ((reinterpret_cast<std::uintptr_t>(page) & 7U) != 0) {
        failInvariant("target-alignment", round, page);
    }
    std::memcpy(page, code.data(), code.size());
    if (::FlushInstructionCache(::GetCurrentProcess(), page, code.size()) == FALSE) {
        failInvariant("target-flush", round, page);
    }
    DWORD old_protection = 0;
    if (::VirtualProtect(page, 64 * 1024, PAGE_EXECUTE_READ, &old_protection) == FALSE) {
        failInvariant("target-rx-protection", round, page);
    }

    // Match the production lifetime invariant: once an entry is published, the
    // entry backing allocation remains valid until process exit. The test
    // intentionally leaves these tiny synthetic targets allocated.
    return page;
}

void waitForCounter(const std::atomic<std::uint64_t> &counter, std::uint64_t expected, const char *label,
                    std::size_t round)
{
    const std::uint64_t deadline = ::GetTickCount64() + kCounterTimeoutMs;
    while (counter.load(std::memory_order_acquire) < expected) {
        if (::GetTickCount64() >= deadline) {
            std::fprintf(stderr,
                         "stage=permanent-gateway-publication counter-timeout label=%s round=%zu phase=%u "
                         "current=%llu expected=%llu entry=%p gateway=%p\n",
                         label, round, g_phase.load(std::memory_order_relaxed),
                         static_cast<unsigned long long>(counter.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(expected), g_entry.load(std::memory_order_relaxed),
                         g_gateway.load(std::memory_order_relaxed));
            std::fflush(stderr);
            std::abort();
        }
        std::this_thread::yield();
    }
}

void verifyPermanentMemoryPolicy(const PermanentGatewayHandle &handle, std::size_t round)
{
    MEMORY_BASIC_INFORMATION code_memory{};
    MEMORY_BASIC_INFORMATION state_memory{};
    if (::VirtualQuery(handle.gateway, &code_memory, sizeof(code_memory)) == 0) {
        failInvariant("gateway-virtualquery", round, handle.entry, handle.gateway);
    }
    if (::VirtualQuery(handle.state, &state_memory, sizeof(state_memory)) == 0) {
        failInvariant("state-virtualquery", round, handle.entry, handle.gateway);
    }
    if (code_memory.State != MEM_COMMIT) {
        failInvariant("gateway-not-committed", round, handle.entry, handle.gateway);
    }
    if (state_memory.State != MEM_COMMIT) {
        failInvariant("state-not-committed", round, handle.entry, handle.gateway);
    }
    if (!isExecutable(code_memory.Protect)) {
        failInvariant("gateway-not-executable", round, handle.entry, handle.gateway);
    }
    if ((code_memory.Protect & 0xFFU) == PAGE_EXECUTE_READWRITE ||
        (code_memory.Protect & 0xFFU) == PAGE_EXECUTE_WRITECOPY) {
        failInvariant("gateway-writable-executable", round, handle.entry, handle.gateway);
    }
    if (isExecutable(state_memory.Protect)) {
        failInvariant("state-executable", round, handle.entry, handle.gateway);
    }
}

void runPublicationRound(std::size_t round)
{
    g_round.store(round, std::memory_order_release);
    g_phase.store(1, std::memory_order_release);
    g_gateway.store(nullptr, std::memory_order_release);

    void *entry = allocateProcessLifetimeTarget(round);
    g_entry.store(entry, std::memory_order_release);
    auto function = reinterpret_cast<SyntheticFn>(entry);
    const int initial_result = function(41);
    if (initial_result != 42) {
        failCallResult("initial", round, 0, 41, initial_result, entry);
    }

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> churn_calls{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            int value = static_cast<int>(worker + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const int actual = function(value);
                if (actual != value + 1) {
                    failCallResult("long-lived", round, worker, value, actual, entry);
                }
                value = (value + 1) & 0x7fff;
                calls.fetch_add(1, std::memory_order_release);
            }
        });
    }

    std::vector<std::thread> churners;
    churners.reserve(kChurners);
    for (std::size_t churner = 0; churner < kChurners; ++churner) {
        churners.emplace_back([&, churner] {
            while (!stop.load(std::memory_order_acquire)) {
                std::thread short_lived([&, churner] {
                    for (std::uint64_t call = 0; call < kChurnBatchCalls; ++call) {
                        const int value = static_cast<int>(call & 0x7fffU);
                        const int actual = function(value);
                        if (actual != value + 1) {
                            failCallResult("thread-churn", round, churner, value, actual, entry);
                        }
                        churn_calls.fetch_add(1, std::memory_order_release);
                    }
                });
                short_lived.join();
            }
        });
    }

    waitForCounter(calls, kPrePublicationCalls, "pre-publication-long-lived", round);
    waitForCounter(churn_calls, kChurnBatchCalls, "pre-publication-thread-churn", round);

    g_phase.store(2, std::memory_order_release);
    PermanentGatewayHandle handle;
    std::string error;
    if (!installPermanentGateway(entry, 0, handle, error)) {
        std::fprintf(stderr,
                     "stage=permanent-gateway-publication install-failure round=%zu error=%s entry=%p gateway=%p\n",
                     round, error.c_str(), entry, handle.gateway);
        std::fflush(stderr);
        std::abort();
    }
    g_gateway.store(handle.gateway, std::memory_order_release);
    if (handle.entry != entry) {
        failInvariant("handle-entry-mismatch", round, entry, handle.gateway);
    }
    if (handle.gateway == nullptr) {
        failInvariant("handle-null-gateway", round, entry);
    }
    if (handle.state == nullptr) {
        failInvariant("handle-null-state", round, entry, handle.gateway);
    }
    if (handle.original == nullptr) {
        failInvariant("handle-null-original", round, entry, handle.gateway);
    }
    verifyPermanentMemoryPolicy(handle, round);

    g_phase.store(3, std::memory_order_release);
    const std::uint64_t post_target = calls.load(std::memory_order_acquire) + kPostPublicationCalls;
    waitForCounter(calls, post_target, "post-publication-long-lived", round);
    const int post_result = function(9);
    if (post_result != 10) {
        failCallResult("post-publication", round, 0, 9, post_result, entry);
    }

    g_phase.store(4, std::memory_order_release);
    PermanentGatewayHandle discovered;
    if (!discoverPermanentGateway(entry, discovered, error)) {
        std::fprintf(stderr,
                     "stage=permanent-gateway-publication rediscovery-failure round=%zu error=%s entry=%p gateway=%p\n",
                     round, error.c_str(), entry, handle.gateway);
        std::fflush(stderr);
        std::abort();
    }
    if (discovered.entry != handle.entry) {
        failInvariant("discovered-entry-mismatch", round, entry, handle.gateway);
    }
    if (discovered.gateway != handle.gateway) {
        failInvariant("discovered-gateway-mismatch", round, entry, handle.gateway);
    }
    if (discovered.state != handle.state) {
        failInvariant("discovered-state-mismatch", round, entry, handle.gateway);
    }
    if (discovered.original != handle.original) {
        failInvariant("discovered-original-mismatch", round, entry, handle.gateway);
    }
    verifyPermanentMemoryPolicy(discovered, round);

    g_phase.store(5, std::memory_order_release);
    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }
    for (std::thread &churner : churners) {
        churner.join();
    }

    if (calls.load(std::memory_order_relaxed) < kPrePublicationCalls + kPostPublicationCalls) {
        failInvariant("long-lived-call-count", round, entry, handle.gateway);
    }
    if (churn_calls.load(std::memory_order_relaxed) < kChurnBatchCalls) {
        failInvariant("thread-churn-call-count", round, entry, handle.gateway);
    }
    if ((round + 1) % 8 == 0) {
        std::fprintf(
            stderr, "stage=permanent-gateway-publication progress=%zu/%zu long_lived_calls=%llu churn_calls=%llu\n",
            round + 1, kPublicationRounds, static_cast<unsigned long long>(calls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(churn_calls.load(std::memory_order_relaxed)));
        std::fflush(stderr);
    }
}

}  // namespace

int main()
{
    ::SetUnhandledExceptionFilter(&publicationUnhandledExceptionFilter);
    std::fprintf(stderr, "stage=permanent-gateway-publication begin rounds=%zu workers=%zu churners=%zu\n",
                 kPublicationRounds, kWorkers, kChurners);
    std::fflush(stderr);
    for (std::size_t round = 0; round < kPublicationRounds; ++round) {
        runPublicationRound(round);
    }
    g_phase.store(6, std::memory_order_release);
    std::fprintf(stderr, "stage=permanent-gateway-publication pass rounds=%zu\n", kPublicationRounds);
    std::fflush(stderr);
    return 0;
}
