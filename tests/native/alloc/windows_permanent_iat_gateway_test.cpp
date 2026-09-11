#include "native/alloc/windows_permanent_iat_gateway.h"

#ifndef _WIN32
#error "windows_permanent_iat_gateway_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using spark::permanent_iat_gateway::bindPermanentIatGateway;
using spark::permanent_iat_gateway::createPermanentIatGateway;
using spark::permanent_iat_gateway::detachPermanentIatGateway;
using spark::permanent_iat_gateway::discoverPermanentIatGateway;
using spark::permanent_iat_gateway::permanentIatGatewayActive;
using spark::permanent_iat_gateway::permanentIatGatewayAdmissionOpen;
using spark::permanent_iat_gateway::permanentIatGatewayGeneration;
using spark::permanent_iat_gateway::PermanentIatGatewayHandle;
using spark::permanent_iat_gateway::permanentIatGatewayHandler;
using spark::permanent_iat_gateway::permanentIatGatewayOriginal;

namespace {

using FiveArgFn = std::uint64_t(__cdecl *)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

constexpr std::size_t KPublicationRounds = 64;
constexpr std::size_t KWorkers = 8;
constexpr std::size_t KChurners = 2;
constexpr std::uint64_t KPrePublicationCalls = 20000;
constexpr std::uint64_t KChurnBatchCalls = 64;
constexpr std::uint64_t KCounterTimeoutMs = 5000;
constexpr std::uint64_t KDrainTimeoutMs = 5000;
constexpr std::size_t KReloadCycles = 1000;
constexpr std::uint64_t KHandlerBias = 0x100000000ULL;
constexpr std::uint64_t KForeignBias = 0x200000000ULL;

std::atomic<std::uint64_t> GHandlerCalls{0};
std::atomic<std::size_t> GRound{0};
std::atomic<unsigned> GPhase{0};
std::atomic<void *> GGateway{nullptr};
std::atomic<std::uintptr_t> GSlot{0};

[[nodiscard]] std::uint64_t baseValue(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                      std::uint64_t e) noexcept
{
    return a + 3 * b + 5 * c + 7 * d + 11 * e;
}

extern "C" __declspec(noinline) std::uint64_t __cdecl originalFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                   std::uint64_t d, std::uint64_t e) noexcept
{
    return baseValue(a, b, c, d, e);
}

extern "C" __declspec(noinline) std::uint64_t __cdecl handlerFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                  std::uint64_t d, std::uint64_t e) noexcept
{
    GHandlerCalls.fetch_add(1, std::memory_order_relaxed);
    return baseValue(a, b, c, d, e) + KHandlerBias;
}

extern "C" __declspec(noinline) std::uint64_t __cdecl foreignFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                  std::uint64_t d, std::uint64_t e) noexcept
{
    return baseValue(a, b, c, d, e) + KForeignBias;
}

[[nodiscard]] std::uintptr_t addressOf(FiveArgFn function) noexcept
{
    return reinterpret_cast<std::uintptr_t>(function);
}

[[nodiscard]] FiveArgFn functionAt(std::uintptr_t address) noexcept
{
    return std::bit_cast<FiveArgFn>(address);
}

[[noreturn]] void fail(const char *reason)
{
    std::fprintf(
        stderr, "stage=permanent-iat-gateway failure=%s round=%zu phase=%u slot=0x%llx gateway=%p handler_calls=%llu\n",
        reason, GRound.load(std::memory_order_relaxed), GPhase.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(GSlot.load(std::memory_order_relaxed)),
        GGateway.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(GHandlerCalls.load(std::memory_order_relaxed)));
    std::fflush(stderr);
    std::abort();
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    const EXCEPTION_RECORD *record = exception != nullptr ? exception->ExceptionRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    const void *exception_address = record != nullptr ? record->ExceptionAddress : nullptr;
    std::uintptr_t rip = 0;
#ifdef _M_X64
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        rip = static_cast<std::uintptr_t>(exception->ContextRecord->Rip);
    }
#endif
    std::fprintf(stderr,
                 "stage=permanent-iat-gateway exception=0x%08lx address=%p rip=0x%llx round=%zu phase=%u "
                 "slot=0x%llx gateway=%p\n",
                 static_cast<unsigned long>(code), exception_address, static_cast<unsigned long long>(rip),
                 GRound.load(std::memory_order_relaxed), GPhase.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(GSlot.load(std::memory_order_relaxed)),
                 GGateway.load(std::memory_order_relaxed));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void waitForCounter(const std::atomic<std::uint64_t> &counter, std::uint64_t expected, const char *label)
{
    const std::uint64_t deadline = ::GetTickCount64() + KCounterTimeoutMs;
    while (counter.load(std::memory_order_acquire) < expected) {
        if (::GetTickCount64() >= deadline) {
            std::fprintf(stderr, "stage=permanent-iat-gateway counter-timeout label=%s current=%llu expected=%llu\n",
                         label, static_cast<unsigned long long>(counter.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(expected));
            std::fflush(stderr);
            std::abort();
        }
        std::this_thread::yield();
    }
}

void requireKnownResult(std::uint64_t result, std::uint64_t expected_base)
{
    if (result != expected_base && result != expected_base + KHandlerBias && result != expected_base + KForeignBias) {
        fail("unknown-call-result");
    }
}

void requireGatewayMemoryPolicy(const PermanentIatGatewayHandle &handle)
{
    MEMORY_BASIC_INFORMATION code{};
    MEMORY_BASIC_INFORMATION state{};
    if (::VirtualQuery(handle.gateway, &code, sizeof(code)) == 0 ||
        ::VirtualQuery(handle.state, &state, sizeof(state)) == 0) {
        fail("VirtualQuery");
    }
    const DWORD code_base = code.Protect & 0xFFU;
    const DWORD state_base = state.Protect & 0xFFU;
    if (code.State != MEM_COMMIT || (code_base != PAGE_EXECUTE && code_base != PAGE_EXECUTE_READ)) {
        fail("gateway-not-rx");
    }
    if (state.State != MEM_COMMIT || (state_base != PAGE_READWRITE && state_base != PAGE_WRITECOPY)) {
        fail("state-not-rw-nx");
    }
}

void runPublicationRound(std::size_t round)
{
    GRound.store(round, std::memory_order_release);
    GPhase.store(1, std::memory_order_release);
    GGateway.store(nullptr, std::memory_order_release);

    alignas(std::uintptr_t) std::uintptr_t slot_storage = addressOf(&originalFive);
    GSlot.store(slot_storage, std::memory_order_release);
    std::atomic_ref<std::uintptr_t> slot(slot_storage);
    if (!slot.is_lock_free()) {
        fail("pointer-slot-not-lock-free");
    }

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> churn_calls{0};

    std::vector<std::thread> workers;
    workers.reserve(KWorkers);
    for (std::size_t worker = 0; worker < KWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            const std::uint64_t a = worker + 1;
            while (!stop.load(std::memory_order_acquire)) {
                const auto function = functionAt(slot.load(std::memory_order_acquire));
                const std::uint64_t expected = baseValue(a, 2, 3, 4, 5);
                requireKnownResult(function(a, 2, 3, 4, 5), expected);
                calls.fetch_add(1, std::memory_order_release);
            }
        });
    }

    std::vector<std::thread> churners;
    churners.reserve(KChurners);
    for (std::size_t churner = 0; churner < KChurners; ++churner) {
        churners.emplace_back([&, churner] {
            while (!stop.load(std::memory_order_acquire)) {
                std::thread short_lived([&, churner] {
                    for (std::uint64_t call = 0; call < KChurnBatchCalls; ++call) {
                        const auto function = functionAt(slot.load(std::memory_order_acquire));
                        const auto a = static_cast<std::uint64_t>(churner + 1);
                        const std::uint64_t expected = baseValue(a, call & 31U, 3, 4, 5);
                        requireKnownResult(function(a, call & 31U, 3, 4, 5), expected);
                        churn_calls.fetch_add(1, std::memory_order_release);
                    }
                });
                short_lived.join();
            }
        });
    }

    waitForCounter(calls, KPrePublicationCalls, "pre-publication-workers");
    waitForCounter(churn_calls, KChurnBatchCalls, "pre-publication-churn");

    GPhase.store(2, std::memory_order_release);
    PermanentIatGatewayHandle handle;
    std::string error;
    if (!createPermanentIatGateway(reinterpret_cast<void *>(&originalFive), 1, handle, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway create-failure round=%zu error=%s\n", round, error.c_str());
        std::fflush(stderr);
        std::abort();
    }
    GGateway.store(handle.gateway, std::memory_order_release);
    requireGatewayMemoryPolicy(handle);
    if (handle.original != reinterpret_cast<void *>(&originalFive) || handle.gateway == nullptr ||
        handle.state == nullptr || handle.stack_argument_count != 1 || permanentIatGatewayAdmissionOpen(handle) ||
        permanentIatGatewayHandler(handle) != nullptr || permanentIatGatewayActive(handle) != 0) {
        fail("created-handle-invariant");
    }

    PermanentIatGatewayHandle discovered;
    if (!discoverPermanentIatGateway(handle.gateway, discovered, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway discover-failure round=%zu error=%s\n", round, error.c_str());
        std::fflush(stderr);
        std::abort();
    }
    if (discovered.gateway != handle.gateway || discovered.state != handle.state ||
        discovered.original != handle.original) {
        fail("discovered-handle-mismatch");
    }

    // This is the proposed production publication primitive: only an aligned,
    // lock-free data pointer changes. The original function's executable bytes
    // are never modified while callers are running.
    GPhase.store(3, std::memory_order_release);
    std::uintptr_t expected_slot = addressOf(&originalFive);
    if (!slot.compare_exchange_strong(expected_slot, reinterpret_cast<std::uintptr_t>(handle.gateway),
                                      std::memory_order_acq_rel, std::memory_order_acquire)) {
        fail("iat-slot-publication-cas");
    }
    GSlot.store(slot.load(std::memory_order_acquire), std::memory_order_release);

    const auto published = functionAt(slot.load(std::memory_order_acquire));
    const std::uint64_t direct_base = baseValue(10, 20, 30, 40, 50);
    if (published(10, 20, 30, 40, 50) != direct_base) {
        fail("closed-gateway-not-original");
    }

    GPhase.store(4, std::memory_order_release);
    if (!bindPermanentIatGateway(discovered, reinterpret_cast<void *>(&handlerFive), KDrainTimeoutMs, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway bind-failure round=%zu error=%s\n", round, error.c_str());
        std::fflush(stderr);
        std::abort();
    }
    if (!permanentIatGatewayAdmissionOpen(discovered) || permanentIatGatewayHandler(discovered) == nullptr) {
        fail("bound-state-invariant");
    }
    for (std::size_t call = 0; call < 256; ++call) {
        if (published(10, 20, 30, 40, 50) != direct_base + KHandlerBias) {
            fail("bound-gateway-not-handler");
        }
    }

    GPhase.store(5, std::memory_order_release);
    if (!detachPermanentIatGateway(discovered, KDrainTimeoutMs, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway detach-failure round=%zu error=%s\n", round, error.c_str());
        std::fflush(stderr);
        std::abort();
    }
    // active may pulse after detach observes zero: a caller can have read the
    // old open gate before close and only increment active afterwards. The
    // changed generation and closed gate force that delayed caller to rollback
    // in permanent code before it can load/call handler. Unload safety therefore
    // requires closed admission + cleared handler here, not permanently-zero
    // active while arbitrary callers keep arriving.
    if (permanentIatGatewayAdmissionOpen(discovered) || permanentIatGatewayHandler(discovered) != nullptr) {
        fail("detached-state-invariant");
    }
    for (std::size_t call = 0; call < 256; ++call) {
        if (published(10, 20, 30, 40, 50) != direct_base) {
            fail("detached-gateway-not-original");
        }
    }

    // Simulate a third party taking over the public slot after Spark detached.
    // Cached old gateway pointers must remain safe pass-through forever.
    GPhase.store(6, std::memory_order_release);
    expected_slot = reinterpret_cast<std::uintptr_t>(handle.gateway);
    if (!slot.compare_exchange_strong(expected_slot, addressOf(&foreignFive), std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        fail("third-party-slot-takeover-cas");
    }
    GSlot.store(slot.load(std::memory_order_acquire), std::memory_order_release);
    if (functionAt(slot.load(std::memory_order_acquire))(10, 20, 30, 40, 50) != direct_base + KForeignBias) {
        fail("third-party-slot-result");
    }
    if (published(10, 20, 30, 40, 50) != direct_base) {
        fail("cached-gateway-after-ownership-loss");
    }

    stop.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }
    for (std::thread &churner : churners) {
        churner.join();
    }
    if (permanentIatGatewayActive(discovered) != 0 || permanentIatGatewayHandler(discovered) != nullptr ||
        permanentIatGatewayAdmissionOpen(discovered)) {
        fail("post-caller-quiescence-invariant");
    }

    if ((round + 1) % 8 == 0) {
        std::fprintf(stderr,
                     "stage=permanent-iat-gateway publication-progress=%zu/%zu worker_calls=%llu churn_calls=%llu "
                     "generation=%llu rx=%zu rw=%zu\n",
                     round + 1, KPublicationRounds,
                     static_cast<unsigned long long>(calls.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(churn_calls.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(permanentIatGatewayGeneration(discovered)),
                     discovered.permanent_rx_bytes, discovered.permanent_rw_bytes);
        std::fflush(stderr);
    }
}

void runReloadReuseStress()
{
    GPhase.store(7, std::memory_order_release);
    PermanentIatGatewayHandle created;
    std::string error;
    if (!createPermanentIatGateway(reinterpret_cast<void *>(&originalFive), 1, created, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway reload-create-failure error=%s\n", error.c_str());
        std::fflush(stderr);
        std::abort();
    }
    GGateway.store(created.gateway, std::memory_order_release);
    const auto gateway = functionAt(reinterpret_cast<std::uintptr_t>(created.gateway));
    const std::uint64_t expected = baseValue(1, 2, 3, 4, 5);

    for (std::size_t cycle = 0; cycle < KReloadCycles; ++cycle) {
        PermanentIatGatewayHandle reloaded;
        if (!discoverPermanentIatGateway(created.gateway, reloaded, error)) {
            std::fprintf(stderr, "stage=permanent-iat-gateway reload-discover-failure cycle=%zu error=%s\n", cycle,
                         error.c_str());
            std::fflush(stderr);
            std::abort();
        }
        if (reloaded.gateway != created.gateway || reloaded.state != created.state ||
            permanentIatGatewayOriginal(reloaded) != reinterpret_cast<void *>(&originalFive)) {
            fail("reload-discovery-identity");
        }
        if (!bindPermanentIatGateway(reloaded, reinterpret_cast<void *>(&handlerFive), KDrainTimeoutMs, error)) {
            std::fprintf(stderr, "stage=permanent-iat-gateway reload-bind-failure cycle=%zu error=%s\n", cycle,
                         error.c_str());
            std::fflush(stderr);
            std::abort();
        }
        if (gateway(1, 2, 3, 4, 5) != expected + KHandlerBias) {
            fail("reload-handler-result");
        }
        if (!detachPermanentIatGateway(reloaded, KDrainTimeoutMs, error)) {
            std::fprintf(stderr, "stage=permanent-iat-gateway reload-detach-failure cycle=%zu error=%s\n", cycle,
                         error.c_str());
            std::fflush(stderr);
            std::abort();
        }
        if (gateway(1, 2, 3, 4, 5) != expected || permanentIatGatewayActive(reloaded) != 0 ||
            permanentIatGatewayHandler(reloaded) != nullptr) {
            fail("reload-detached-result");
        }
        if ((cycle + 1) % 100 == 0) {
            std::fprintf(stderr, "stage=permanent-iat-gateway reload-progress=%zu/%zu generation=%llu active=%llu\n",
                         cycle + 1, KReloadCycles,
                         static_cast<unsigned long long>(permanentIatGatewayGeneration(reloaded)),
                         static_cast<unsigned long long>(permanentIatGatewayActive(reloaded)));
            std::fflush(stderr);
        }
    }
}

}  // namespace

int main()
{
    ::SetUnhandledExceptionFilter(&unhandledExceptionFilter);
    std::fprintf(
        stderr, "stage=permanent-iat-gateway begin publication_rounds=%zu workers=%zu churners=%zu reload_cycles=%zu\n",
        KPublicationRounds, KWorkers, KChurners, KReloadCycles);
    std::fflush(stderr);

    for (std::size_t round = 0; round < KPublicationRounds; ++round) {
        runPublicationRound(round);
    }
    runReloadReuseStress();

    GPhase.store(8, std::memory_order_release);
    std::fprintf(stderr,
                 "stage=permanent-iat-gateway pass publication_rounds=%zu reload_cycles=%zu handler_calls=%llu\n",
                 KPublicationRounds, KReloadCycles,
                 static_cast<unsigned long long>(GHandlerCalls.load(std::memory_order_relaxed)));
    std::fflush(stderr);
    return 0;
}
