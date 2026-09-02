#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_experiment.cpp must only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native/alloc/windows_stable_entry_atomic.h"
#include "native/alloc/windows_stable_entry_relocator.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace spark::stable_entry_experiment {
namespace {

constexpr std::uint64_t kGatewayMagic = 0x31595754474B5053ULL;  // "SPKGWTY1" little-endian marker.
constexpr std::uint32_t kGatewayAbiVersion = 1;
constexpr std::size_t kGatewayCodeOffset = 256;
constexpr std::size_t kGatewayCodeSize = 63;
constexpr std::uint64_t kGateClosed = 0;
constexpr std::uint64_t kGateOpen = 1;

struct alignas(64) GatewayState {
    std::uint64_t magic = kGatewayMagic;
    std::uint32_t abi_version = kGatewayAbiVersion;
    std::uint32_t struct_size = sizeof(GatewayState);
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> gate{kGateClosed};
    std::atomic<std::uint64_t> active{0};
    std::atomic<void *> handler{nullptr};
    void *original = nullptr;
    void *entry = nullptr;
    void *gateway = nullptr;
    std::uint64_t code_hash = 0;
    std::uint64_t original_hash = 0;
    std::uint64_t installed_hash = 0;
    std::array<std::uint8_t, 8> original_bytes{};
    std::array<std::uint8_t, 8> installed_bytes{};
};

static_assert(offsetof(GatewayState, generation) == 16);
static_assert(offsetof(GatewayState, gate) == 24);
static_assert(offsetof(GatewayState, active) == 32);
static_assert(offsetof(GatewayState, handler) == 40);
static_assert(offsetof(GatewayState, original) == 48);

[[nodiscard]] std::uint64_t hashBytesLocal(const void *data, std::size_t size) noexcept
{
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t value = offset;
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (std::size_t index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= prime;
    }
    return value;
}

[[nodiscard]] bool dynamicCodeAllowed(std::string &error) noexcept
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &policy,
                                     static_cast<SIZE_T>(sizeof(policy))) == FALSE) {
        error = "GetProcessMitigationPolicy(ProcessDynamicCodePolicy) failed: " +
                std::to_string(::GetLastError());
        return false;
    }
    if (policy.ProhibitDynamicCode != 0) {
        error = "ProcessDynamicCodePolicy prohibits permanent gateway executable code";
        return false;
    }
    return true;
}

[[nodiscard]] std::array<std::uint8_t, kGatewayCodeSize> gatewayCode(GatewayState *state) noexcept
{
    // Windows x64 ABI preserving tail gateway. It touches only volatile R10/R11
    // and does not call a helper, so allocator arguments and stack arguments are
    // unchanged when execution reaches either destination.
    //
    //   mov r11, state
    //   cmp [r11+gate], 0
    //   je  fallback
    //   mov r10, [r11+generation]
    //   lock inc [r11+active]
    //   cmp [r11+gate], 0
    //   je  rollback
    //   cmp r10, [r11+generation]
    //   jne rollback
    //   mov r10, [r11+handler]
    //   test r10, r10
    //   je  rollback
    //   jmp r10
    // rollback:
    //   lock dec [r11+active]
    // fallback:
    //   mov r10, [r11+original]
    //   jmp r10
    std::array<std::uint8_t, kGatewayCodeSize> code{
        0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,
        0x49, 0x83, 0x7B, 0x18, 0x00,
        0x74, 0x27,
        0x4D, 0x8B, 0x53, 0x10,
        0xF0, 0x49, 0xFF, 0x43, 0x20,
        0x49, 0x83, 0x7B, 0x18, 0x00,
        0x74, 0x12,
        0x4D, 0x3B, 0x53, 0x10,
        0x75, 0x0C,
        0x4D, 0x8B, 0x53, 0x28,
        0x4D, 0x85, 0xD2,
        0x74, 0x03,
        0x41, 0xFF, 0xE2,
        0xF0, 0x49, 0xFF, 0x4B, 0x20,
        0x4D, 0x8B, 0x53, 0x30,
        0x41, 0xFF, 0xE2,
    };
    const std::uint64_t state_address = reinterpret_cast<std::uint64_t>(state);
    std::memcpy(code.data() + 2, &state_address, sizeof(state_address));
    return code;
}

[[nodiscard]] bool isExecutableReadOnly(DWORD protection) noexcept
{
    const DWORD base = protection & 0xFFU;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ;
}

[[nodiscard]] bool isWritableNonExecutable(DWORD protection) noexcept
{
    const DWORD base = protection & 0xFFU;
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY;
}

[[nodiscard]] bool queryCommitted(void *address, MEMORY_BASIC_INFORMATION &memory, std::string &error)
{
    memory = {};
    if (address == nullptr || ::VirtualQuery(address, &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT || memory.BaseAddress == nullptr || memory.RegionSize == 0) {
        error = "permanent gateway address is not committed memory";
        return false;
    }
    return true;
}

[[nodiscard]] GatewayState *stateFromHandle(const PermanentGatewayHandle &handle) noexcept
{
    return static_cast<GatewayState *>(handle.state);
}

[[nodiscard]] bool waitForZero(GatewayState *state, std::uint64_t timeout_ms, std::string &error)
{
    const std::uint64_t deadline = ::GetTickCount64() + timeout_ms;
    while (state->active.load(std::memory_order_acquire) != 0) {
        if (::GetTickCount64() >= deadline) {
            error = "timed out draining permanent gateway admitted callbacks";
            return false;
        }
        ::Sleep(0);
    }
    return true;
}

[[nodiscard]] bool validateStateIdentity(GatewayState *state, void *entry, void *gateway, std::string &error)
{
    if (state == nullptr || state->magic != kGatewayMagic || state->abi_version != kGatewayAbiVersion ||
        state->struct_size != sizeof(GatewayState) || state->entry != entry || state->gateway != gateway ||
        state->original == nullptr) {
        error = "permanent gateway state identity/ABI validation failed";
        return false;
    }
    return true;
}

void populateHandle(GatewayState *state, MEMORY_BASIC_INFORMATION code_memory,
                    MEMORY_BASIC_INFORMATION state_memory, PermanentGatewayHandle &handle) noexcept
{
    handle.entry = state->entry;
    handle.gateway = state->gateway;
    handle.original = state->original;
    handle.state = state;
    handle.permanent_rx_bytes = code_memory.RegionSize;
    handle.permanent_rw_bytes = state_memory.RegionSize;
    handle.generation = state->generation.load(std::memory_order_acquire);
}

}  // namespace

bool installPermanentGateway(void *entry, PermanentGatewayHandle &handle, std::string &error)
{
    handle = {};
    error.clear();
    if (entry == nullptr) {
        error = "permanent gateway entry is null";
        return false;
    }
    if (!dynamicCodeAllowed(error)) {
        return false;
    }
    if (!isAlignedForAtomic8(reinterpret_cast<std::uintptr_t>(entry))) {
        error = "current permanent gateway prototype requires an 8-byte aligned entry";
        return false;
    }

    MEMORY_BASIC_INFORMATION entry_memory{};
    if (!queryCommitted(entry, entry_memory, error) || !isExecutableReadOnly(entry_memory.Protect)) {
        if (error.empty()) {
            error = "permanent gateway entry is not executable read-only memory";
        }
        return false;
    }
    const auto entry_value = reinterpret_cast<std::uintptr_t>(entry);
    const auto region_end = reinterpret_cast<std::uintptr_t>(entry_memory.BaseAddress) + entry_memory.RegionSize;
    if (entry_value > region_end || region_end - entry_value < 16) {
        error = "permanent gateway entry does not expose a readable 16-byte decode window";
        return false;
    }

    std::array<std::uint8_t, 16> original{};
    std::memcpy(original.data(), entry, original.size());

    BoundedRelocation relocation;
    if (!prepareBoundedRelocation(entry, relocation, error)) {
        return false;
    }
    if (relocation.memory == nullptr || relocation.entry == nullptr ||
        relocation.allocation_size < kGatewayCodeOffset + kGatewayCodeSize ||
        !rel32Reachable(entry_value + 5,
                        reinterpret_cast<std::uintptr_t>(relocation.memory) + kGatewayCodeOffset)) {
        releaseBoundedRelocation(relocation);
        error = "bounded relocation did not provide a rel32-reachable permanent code island";
        return false;
    }

    void *state_memory_raw = ::VirtualAlloc(nullptr, sizeof(GatewayState), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (state_memory_raw == nullptr) {
        const DWORD failure = ::GetLastError();
        releaseBoundedRelocation(relocation);
        error = "VirtualAlloc permanent gateway state failed: " + std::to_string(failure);
        return false;
    }
    auto *state = ::new (state_memory_raw) GatewayState{};
    state->entry = entry;
    state->original = relocation.entry;
    state->gateway = static_cast<std::uint8_t *>(relocation.memory) + kGatewayCodeOffset;
    std::memcpy(state->original_bytes.data(), original.data(), state->original_bytes.size());
    state->original_hash = hashBytesLocal(state->original_bytes.data(), state->original_bytes.size());

    const auto code = gatewayCode(state);
    DWORD old_code_protection = 0;
    if (::VirtualProtect(relocation.memory, relocation.allocation_size, PAGE_READWRITE, &old_code_protection) == FALSE) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        releaseBoundedRelocation(relocation);
        error = "VirtualProtect permanent code island writable failed: " + std::to_string(failure);
        return false;
    }
    std::memcpy(state->gateway, code.data(), code.size());
    DWORD ignored_code_protection = 0;
    if (::VirtualProtect(relocation.memory, relocation.allocation_size, PAGE_EXECUTE_READ,
                         &ignored_code_protection) == FALSE ||
        ::FlushInstructionCache(::GetCurrentProcess(), relocation.memory, relocation.allocation_size) == FALSE) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        releaseBoundedRelocation(relocation);
        error = "publishing permanent RX code island failed: " + std::to_string(failure);
        return false;
    }
    state->code_hash = hashBytesLocal(state->gateway, code.size());

    std::array<std::uint8_t, 16> installed{};
    if (!encodeAtomic8RelayEntry(entry_value, reinterpret_cast<std::uintptr_t>(state->gateway), original, installed,
                                 error)) {
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        releaseBoundedRelocation(relocation);
        return false;
    }
    std::memcpy(state->installed_bytes.data(), installed.data(), state->installed_bytes.size());
    state->installed_hash = hashBytesLocal(state->installed_bytes.data(), state->installed_bytes.size());

    DWORD old_entry_protection = 0;
    if (::VirtualProtect(entry, 8, PAGE_EXECUTE_READWRITE, &old_entry_protection) == FALSE) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        releaseBoundedRelocation(relocation);
        error = "VirtualProtect permanent entry transaction failed: " + std::to_string(failure);
        return false;
    }

    const AtomicCompareResult transaction = atomicCompareExchange8(entry, original, installed);
    const BOOL flushed = transaction.exchanged ? ::FlushInstructionCache(::GetCurrentProcess(), entry, 8) : TRUE;
    DWORD ignored_entry_protection = 0;
    const BOOL protection_restored =
        ::VirtualProtect(entry, 8, old_entry_protection, &ignored_entry_protection);

    if (!transaction.exchanged) {
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        releaseBoundedRelocation(relocation);
        error = "permanent gateway install lost original entry ownership";
        return false;
    }

    // After publication the entry can execute this state/code at any instant.
    // Never reclaim them on a post-CAS failure.
    if (flushed == FALSE || protection_restored == FALSE) {
        error = "permanent gateway entry published but cache/protection restoration failed; resources pinned fail-closed";
        return false;
    }

    MEMORY_BASIC_INFORMATION code_memory{};
    MEMORY_BASIC_INFORMATION state_memory{};
    if (!queryCommitted(state->gateway, code_memory, error) || !isExecutableReadOnly(code_memory.Protect) ||
        !queryCommitted(state, state_memory, error) || !isWritableNonExecutable(state_memory.Protect)) {
        error = "published permanent gateway memory protections violate W^X invariant";
        return false;
    }

    populateHandle(state, code_memory, state_memory, handle);
    return true;
}

bool discoverPermanentGateway(void *entry, PermanentGatewayHandle &handle, std::string &error)
{
    handle = {};
    error.clear();
    if (entry == nullptr) {
        error = "permanent gateway discovery entry is null";
        return false;
    }

    MEMORY_BASIC_INFORMATION entry_memory{};
    if (!queryCommitted(entry, entry_memory, error)) {
        return false;
    }
    const auto entry_value = reinterpret_cast<std::uintptr_t>(entry);
    const auto region_end = reinterpret_cast<std::uintptr_t>(entry_memory.BaseAddress) + entry_memory.RegionSize;
    if (entry_value > region_end || region_end - entry_value < 8) {
        error = "permanent gateway discovery cannot read the 8-byte entry transaction";
        return false;
    }

    std::array<std::uint8_t, 8> installed{};
    std::memcpy(installed.data(), entry, installed.size());
    if (installed[0] != 0xE9) {
        error = "allocator entry does not carry the Spark permanent rel32 signature";
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, installed.data() + 1, sizeof(displacement));
    const std::uintptr_t gateway_value = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(entry_value + 5) + static_cast<std::intptr_t>(displacement));
    auto *gateway = reinterpret_cast<std::uint8_t *>(gateway_value);

    MEMORY_BASIC_INFORMATION code_memory{};
    if (!queryCommitted(gateway, code_memory, error) || !isExecutableReadOnly(code_memory.Protect)) {
        error = "decoded permanent gateway is not committed RX memory";
        return false;
    }
    const auto code_region_end = reinterpret_cast<std::uintptr_t>(code_memory.BaseAddress) + code_memory.RegionSize;
    if (gateway_value > code_region_end || code_region_end - gateway_value < kGatewayCodeSize ||
        gateway[0] != 0x49 || gateway[1] != 0xBB) {
        error = "decoded gateway does not contain the Spark permanent gateway signature";
        return false;
    }

    std::uint64_t state_value = 0;
    std::memcpy(&state_value, gateway + 2, sizeof(state_value));
    auto *state = reinterpret_cast<GatewayState *>(state_value);
    MEMORY_BASIC_INFORMATION state_memory{};
    if (!queryCommitted(state, state_memory, error) || !isWritableNonExecutable(state_memory.Protect)) {
        error = "decoded permanent gateway state is not committed non-executable writable memory";
        return false;
    }
    if (!validateStateIdentity(state, entry, gateway, error)) {
        return false;
    }

    const auto expected_code = gatewayCode(state);
    if (std::memcmp(gateway, expected_code.data(), expected_code.size()) != 0 ||
        state->code_hash != hashBytesLocal(gateway, expected_code.size())) {
        error = "permanent gateway code signature/hash validation failed";
        return false;
    }
    if (std::memcmp(installed.data(), state->installed_bytes.data(), installed.size()) != 0 ||
        state->installed_hash != hashBytesLocal(installed.data(), installed.size()) ||
        state->original_hash != hashBytesLocal(state->original_bytes.data(), state->original_bytes.size())) {
        error = "permanent gateway entry/original metadata hash validation failed";
        return false;
    }

    MEMORY_BASIC_INFORMATION original_memory{};
    if (!queryCommitted(state->original, original_memory, error) || !isExecutableReadOnly(original_memory.Protect)) {
        error = "permanent original trampoline is no longer committed RX memory";
        return false;
    }

    populateHandle(state, code_memory, state_memory, handle);
    return true;
}

bool bindPermanentGateway(PermanentGatewayHandle &handle, void *handler, std::uint64_t timeout_ms,
                          std::string &error)
{
    error.clear();
    auto *state = stateFromHandle(handle);
    if (state == nullptr || handler == nullptr || !validateStateIdentity(state, handle.entry, handle.gateway, error)) {
        if (error.empty()) {
            error = "permanent gateway bind received a null state or handler";
        }
        return false;
    }
    if (state->gate.load(std::memory_order_acquire) != kGateClosed ||
        state->handler.load(std::memory_order_acquire) != nullptr) {
        error = "permanent gateway bind requires a detached state";
        return false;
    }
    if (!waitForZero(state, timeout_ms, error)) {
        return false;
    }

    const std::uint64_t generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    state->handler.store(handler, std::memory_order_release);
    state->gate.store(kGateOpen, std::memory_order_seq_cst);
    handle.generation = generation;
    return true;
}

bool detachPermanentGateway(PermanentGatewayHandle &handle, std::uint64_t timeout_ms, std::string &error)
{
    error.clear();
    auto *state = stateFromHandle(handle);
    if (state == nullptr || !validateStateIdentity(state, handle.entry, handle.gateway, error)) {
        if (error.empty()) {
            error = "permanent gateway detach received a null state";
        }
        return false;
    }

    state->gate.store(kGateClosed, std::memory_order_seq_cst);
    const std::uint64_t generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    handle.generation = generation;
    if (!waitForZero(state, timeout_ms, error)) {
        return false;
    }
    state->handler.store(nullptr, std::memory_order_seq_cst);
    if (state->active.load(std::memory_order_acquire) != 0) {
        error = "permanent gateway active count changed while clearing detached handler";
        return false;
    }
    return true;
}

void *permanentGatewayOriginal(const PermanentGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->original : nullptr;
}

void completePermanentGatewayHandlerCall(const PermanentGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    if (state != nullptr) {
        state->active.fetch_sub(1, std::memory_order_release);
    }
}

void *permanentGatewayHandler(const PermanentGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->handler.load(std::memory_order_acquire) : nullptr;
}

std::uint64_t permanentGatewayActive(const PermanentGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->active.load(std::memory_order_acquire) : 0;
}

std::uint64_t permanentGatewayGeneration(const PermanentGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->generation.load(std::memory_order_acquire) : 0;
}

bool permanentGatewayAdmissionOpen(const PermanentGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr && state->gate.load(std::memory_order_acquire) == kGateOpen;
}

}  // namespace spark::stable_entry_experiment
