#include "native/alloc/windows_stable_entry_permanent_gateway.h"

#ifndef _WIN32
#error "windows_stable_entry_permanent_gateway.cpp must only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "native/alloc/windows_stable_entry_atomic.h"
#include "native/alloc/windows_stable_entry_relocator.h"

namespace spark::stable_entry_experiment {
namespace {

constexpr std::uint64_t kHeaderMagic = 0x3159415747505353ULL;  // "SSPGWAY1"
constexpr std::uint64_t kStateMagic = 0x3154455453475053ULL;   // "SPGSTATE1" (truncated)
constexpr std::uint32_t kGatewayAbiVersion = 1;
constexpr std::size_t kIslandReservationSize = 64 * 1024;
constexpr std::size_t kGatewayOffset = 0x100;
constexpr std::size_t kGatewayCapacity = 512;
constexpr std::uint64_t kCountMask = 0x00000000FFFFFFFFULL;
constexpr std::uint64_t kClosedBit = 0x0000000100000000ULL;
constexpr unsigned kEpochShift = 33;
constexpr std::uint64_t kMaxEpoch = 0x7FFFFFFFULL;

struct alignas(64) PermanentGatewayState {
    volatile LONG64 gate = static_cast<LONG64>(kClosedBit);
    void *volatile handler = nullptr;
    void *original = nullptr;
    std::uint64_t magic = kStateMagic;
    std::uint32_t abi_version = kGatewayAbiVersion;
    std::uint32_t struct_size = sizeof(PermanentGatewayState);
    void *entry = nullptr;
    void *gateway = nullptr;
    void *trampoline = nullptr;
    std::uint64_t island_reserved = 0;
    std::uint64_t island_committed = 0;
    std::uint64_t trampoline_allocation = 0;
};

static_assert(offsetof(PermanentGatewayState, gate) == 0);
static_assert(offsetof(PermanentGatewayState, handler) == 8);
static_assert(offsetof(PermanentGatewayState, original) == 16);

struct PermanentGatewayHeader {
    std::uint64_t magic = kHeaderMagic;
    std::uint32_t abi_version = kGatewayAbiVersion;
    std::uint32_t header_size = sizeof(PermanentGatewayHeader);
    std::uint32_t gateway_offset = static_cast<std::uint32_t>(kGatewayOffset);
    std::uint32_t gateway_size = 0;
    std::uint32_t arity = 0;
    std::uint32_t reserved = 0;
    void *state = nullptr;
    void *entry = nullptr;
    std::uint64_t code_hash = 0;
    std::array<std::uint8_t, 8> original{};
};

static_assert(sizeof(PermanentGatewayHeader) <= kGatewayOffset);

constexpr std::array<std::uint8_t, 139> kGatewayFourTemplate{
    0x48, 0x83, 0xEC, 0x58, 0x48, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x28, 0x4C, 0x89,
    0x44, 0x24, 0x30, 0x4C, 0x89, 0x4C, 0x24, 0x38, 0x49, 0xBA, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
    0x22, 0x11, 0x4C, 0x89, 0x54, 0x24, 0x40, 0x49, 0x8B, 0x02, 0x48, 0x0F, 0xBA, 0xE0, 0x20, 0x72,
    0x3C, 0x48, 0x8D, 0x50, 0x01, 0xF0, 0x49, 0x0F, 0xB1, 0x12, 0x75, 0xEB, 0x48, 0x8B, 0x4C, 0x24,
    0x20, 0x48, 0x8B, 0x54, 0x24, 0x28, 0x4C, 0x8B, 0x44, 0x24, 0x30, 0x4C, 0x8B, 0x4C, 0x24, 0x38, 0x49,
    0x8B, 0x42, 0x08, 0x48, 0x85, 0xC0, 0x75, 0x04, 0x49, 0x8B, 0x42, 0x10, 0xFF, 0xD0, 0x4C, 0x8B,
    0x54, 0x24, 0x40, 0xF0, 0x49, 0xFF, 0x0A, 0x48, 0x83, 0xC4, 0x58, 0xC3, 0x49, 0x8B, 0x42, 0x10,
    0x48, 0x8B, 0x4C, 0x24, 0x20, 0x48, 0x8B, 0x54, 0x24, 0x28, 0x4C, 0x8B, 0x44, 0x24, 0x30, 0x4C,
    0x8B, 0x4C, 0x24, 0x38, 0x48, 0x83, 0xC4, 0x58, 0xFF, 0xE0,
};
constexpr std::size_t kFourStateImmediateOffset = 26;

constexpr std::array<std::uint8_t, 152> kGatewayFiveTemplate{
    0x48, 0x83, 0xEC, 0x68, 0x48, 0x89, 0x4C, 0x24, 0x28, 0x48, 0x89, 0x54, 0x24, 0x30, 0x4C, 0x89,
    0x44, 0x24, 0x38, 0x4C, 0x89, 0x4C, 0x24, 0x40, 0x48, 0x8B, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00,
    0x48, 0x89, 0x44, 0x24, 0x20, 0x49, 0xBA, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x4C,
    0x89, 0x54, 0x24, 0x48, 0x49, 0x8B, 0x02, 0x48, 0x0F, 0xBA, 0xE0, 0x20, 0x72, 0x3C, 0x48, 0x8D,
    0x50, 0x01, 0xF0, 0x49, 0x0F, 0xB1, 0x12, 0x75, 0xEB, 0x48, 0x8B, 0x4C, 0x24, 0x28, 0x48, 0x8B,
    0x54, 0x24, 0x30, 0x4C, 0x8B, 0x44, 0x24, 0x38, 0x4C, 0x8B, 0x4C, 0x24, 0x40, 0x49, 0x8B, 0x42,
    0x08, 0x48, 0x85, 0xC0, 0x75, 0x04, 0x49, 0x8B, 0x42, 0x10, 0xFF, 0xD0, 0x4C, 0x8B, 0x54, 0x24,
    0x48, 0xF0, 0x49, 0xFF, 0x0A, 0x48, 0x83, 0xC4, 0x68, 0xC3, 0x49, 0x8B, 0x42, 0x10, 0x48, 0x8B,
    0x4C, 0x24, 0x28, 0x48, 0x8B, 0x54, 0x24, 0x30, 0x4C, 0x8B, 0x44, 0x24, 0x38, 0x4C, 0x8B, 0x4C,
    0x24, 0x40, 0x48, 0x83, 0xC4, 0x68, 0xFF, 0xE0,
};
constexpr std::size_t kFiveStateImmediateOffset = 39;

struct IslandBuild {
    void *reservation = nullptr;
    PermanentGatewayHeader *header = nullptr;
    PermanentGatewayState *state = nullptr;
    void *gateway = nullptr;
    std::size_t page_size = 0;
    std::size_t code_size = 0;
};

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool executableProtection(DWORD protection) noexcept
{
    protection &= 0xFFU;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool dynamicCodeAllowed(std::string &error) noexcept
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamic_policy{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &dynamic_policy,
                                     sizeof(dynamic_policy)) == FALSE) {
        error = "GetProcessMitigationPolicy(ProcessDynamicCodePolicy) failed: " + std::to_string(::GetLastError());
        return false;
    }
    if (dynamic_policy.ProhibitDynamicCode != 0) {
        error = "ProcessDynamicCodePolicy prohibits permanent gateway executable memory";
        return false;
    }

    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg_policy{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg_policy,
                                     sizeof(cfg_policy)) == FALSE) {
        error = "GetProcessMitigationPolicy(ProcessControlFlowGuardPolicy) failed: " + std::to_string(::GetLastError());
        return false;
    }
    if (cfg_policy.EnableControlFlowGuard != 0) {
        // The hand-built prototype has not yet emitted the compiler CFG dispatch
        // check sequence. Treat CFG as unsupported rather than silently bypassing
        // a process mitigation.
        error = "CFG is enabled; permanent gateway prototype fails closed until CFG dispatch is implemented";
        return false;
    }
    return true;
}

bool buildGatewayCode(PermanentGatewayArity arity, PermanentGatewayState *state,
                      std::array<std::uint8_t, kGatewayCapacity> &code, std::size_t &size,
                      std::string &error) noexcept
{
    code.fill(0);
    const std::uintptr_t state_address = reinterpret_cast<std::uintptr_t>(state);
    if (arity == PermanentGatewayArity::UpToFourIntegerArgs) {
        size = kGatewayFourTemplate.size();
        std::memcpy(code.data(), kGatewayFourTemplate.data(), size);
        std::memcpy(code.data() + kFourStateImmediateOffset, &state_address, sizeof(state_address));
        return true;
    }
    if (arity == PermanentGatewayArity::FiveIntegerArgs) {
        size = kGatewayFiveTemplate.size();
        std::memcpy(code.data(), kGatewayFiveTemplate.data(), size);
        std::memcpy(code.data() + kFiveStateImmediateOffset, &state_address, sizeof(state_address));
        return true;
    }
    error = "unsupported permanent gateway integer argument arity";
    return false;
}

void releaseUnpublishedIsland(IslandBuild &island) noexcept
{
    if (island.reservation != nullptr) {
        (void)::VirtualFree(island.reservation, 0, MEM_RELEASE);
    }
    island = {};
}

bool reserveIslandNear(void *entry, PermanentGatewayArity arity, const BoundedRelocation &relocation,
                       const std::array<std::uint8_t, 16> &original, IslandBuild &island, std::string &error)
{
    SYSTEM_INFO system{};
    ::GetSystemInfo(&system);
    const std::uintptr_t granularity =
        system.dwAllocationGranularity != 0 ? system.dwAllocationGranularity : kIslandReservationSize;
    const std::size_t page_size = system.dwPageSize != 0 ? system.dwPageSize : 4096;
    if (page_size * 2 > kIslandReservationSize || kGatewayOffset + kGatewayCapacity > page_size) {
        error = "Windows page geometry cannot host permanent gateway RX/RW split";
        return false;
    }

    const std::uintptr_t entry_address = reinterpret_cast<std::uintptr_t>(entry);
    const std::uintptr_t base = entry_address & ~(granularity - 1);
    const std::uintptr_t max_distance = static_cast<std::uintptr_t>(INT32_MAX) - granularity;

    for (std::uintptr_t distance = 0; distance <= max_distance; distance += granularity) {
        const std::uintptr_t candidates[2] = {
            base >= distance ? base - distance : 0,
            base <= (std::numeric_limits<std::uintptr_t>::max)() - distance ? base + distance : 0,
        };
        for (std::uintptr_t candidate : candidates) {
            if (candidate == 0) {
                continue;
            }
            const std::uintptr_t gateway_address = candidate + kGatewayOffset;
            if (!rel32Reachable(entry_address + 5, gateway_address)) {
                continue;
            }

            void *reservation =
                ::VirtualAlloc(reinterpret_cast<void *>(candidate), kIslandReservationSize, MEM_RESERVE, PAGE_NOACCESS);
            if (reservation == nullptr || reinterpret_cast<std::uintptr_t>(reservation) != candidate) {
                if (reservation != nullptr) {
                    (void)::VirtualFree(reservation, 0, MEM_RELEASE);
                }
                continue;
            }
            void *rx_page = ::VirtualAlloc(reservation, page_size, MEM_COMMIT, PAGE_READWRITE);
            void *rw_page = ::VirtualAlloc(static_cast<std::uint8_t *>(reservation) + page_size, page_size,
                                           MEM_COMMIT, PAGE_READWRITE);
            if (rx_page == nullptr || rw_page == nullptr) {
                (void)::VirtualFree(reservation, 0, MEM_RELEASE);
                continue;
            }

            auto *header = static_cast<PermanentGatewayHeader *>(rx_page);
            auto *state = static_cast<PermanentGatewayState *>(rw_page);
            void *gateway = static_cast<std::uint8_t *>(rx_page) + kGatewayOffset;
            *state = {};
            state->gate = static_cast<LONG64>(kClosedBit);
            state->handler = nullptr;
            state->original = relocation.entry;
            state->magic = kStateMagic;
            state->abi_version = kGatewayAbiVersion;
            state->struct_size = sizeof(PermanentGatewayState);
            state->entry = entry;
            state->gateway = gateway;
            state->trampoline = relocation.entry;
            state->island_reserved = kIslandReservationSize;
            state->island_committed = page_size * 2;
            state->trampoline_allocation = relocation.allocation_size;

            std::array<std::uint8_t, kGatewayCapacity> code{};
            std::size_t code_size = 0;
            if (!buildGatewayCode(arity, state, code, code_size, error)) {
                (void)::VirtualFree(reservation, 0, MEM_RELEASE);
                return false;
            }
            std::memcpy(gateway, code.data(), code_size);

            *header = {};
            header->magic = kHeaderMagic;
            header->abi_version = kGatewayAbiVersion;
            header->header_size = sizeof(PermanentGatewayHeader);
            header->gateway_offset = static_cast<std::uint32_t>(kGatewayOffset);
            header->gateway_size = static_cast<std::uint32_t>(code_size);
            header->arity = static_cast<std::uint32_t>(arity);
            header->state = state;
            header->entry = entry;
            header->code_hash = fnv1a(std::span<const std::uint8_t>(code.data(), code_size));
            std::memcpy(header->original.data(), original.data(), header->original.size());

            DWORD old_protection = 0;
            if (::VirtualProtect(rx_page, page_size, PAGE_EXECUTE_READ, &old_protection) == FALSE) {
                const DWORD failure = ::GetLastError();
                (void)::VirtualFree(reservation, 0, MEM_RELEASE);
                error = "VirtualProtect permanent gateway RX page failed: " + std::to_string(failure);
                return false;
            }
            if (::FlushInstructionCache(::GetCurrentProcess(), rx_page, page_size) == FALSE) {
                const DWORD failure = ::GetLastError();
                (void)::VirtualFree(reservation, 0, MEM_RELEASE);
                error = "FlushInstructionCache permanent gateway failed: " + std::to_string(failure);
                return false;
            }

            island.reservation = reservation;
            island.header = header;
            island.state = state;
            island.gateway = gateway;
            island.page_size = page_size;
            island.code_size = code_size;
            return true;
        }
        if (max_distance - distance < granularity) {
            break;
        }
    }

    error = "could not reserve rel32-reachable permanent gateway island";
    return false;
}

bool publishEntry(void *entry, const std::array<std::uint8_t, 16> &original, void *gateway, bool &published,
                  std::string &error) noexcept
{
    published = false;
    std::array<std::uint8_t, 16> installed{};
    if (!encodeAtomic8RelayEntry(reinterpret_cast<std::uintptr_t>(entry), reinterpret_cast<std::uintptr_t>(gateway),
                                 original, installed, error)) {
        return false;
    }

    DWORD old_protection = 0;
    if (::VirtualProtect(entry, kAtomicEntryWidth8, PAGE_EXECUTE_READWRITE, &old_protection) == FALSE) {
        error = "VirtualProtect before permanent gateway publish failed: " + std::to_string(::GetLastError());
        return false;
    }
    const AtomicCompareResult exchanged = atomicCompareExchange8(entry, original, installed);
    if (!exchanged.exchanged) {
        DWORD ignored = 0;
        const BOOL restored = ::VirtualProtect(entry, kAtomicEntryWidth8, old_protection, &ignored);
        error = restored != FALSE ? "stable entry ownership changed before permanent gateway publish"
                                  : "stable entry ownership changed and entry protection restore failed";
        return false;
    }
    published = true;

    if (::FlushInstructionCache(::GetCurrentProcess(), entry, kAtomicEntryWidth8) == FALSE) {
        error = "FlushInstructionCache after permanent gateway publish failed: " + std::to_string(::GetLastError());
        return false;
    }
    DWORD ignored = 0;
    if (::VirtualProtect(entry, kAtomicEntryWidth8, old_protection, &ignored) == FALSE) {
        error = "VirtualProtect restore after permanent gateway publish failed: " + std::to_string(::GetLastError());
        return false;
    }
    return true;
}

std::uint64_t gateRead(const PermanentGatewayState *state) noexcept
{
    return static_cast<std::uint64_t>(::InterlockedCompareExchange64(
        const_cast<volatile LONG64 *>(&state->gate), static_cast<LONG64>(0), static_cast<LONG64>(0)));
}

bool stateClosed(std::uint64_t state) noexcept
{
    return (state & kClosedBit) != 0;
}

std::uint32_t stateCount(std::uint64_t state) noexcept
{
    return static_cast<std::uint32_t>(state & kCountMask);
}

std::uint32_t stateGeneration(std::uint64_t state) noexcept
{
    return static_cast<std::uint32_t>(state >> kEpochShift);
}

void *handlerRead(PermanentGatewayState *state) noexcept
{
    return ::InterlockedCompareExchangePointer(&state->handler, nullptr, nullptr);
}

bool validatePermanentState(PermanentGatewayState *state, void *entry, void *gateway,
                            PermanentGatewayArity arity, const PermanentGatewayHeader &header,
                            std::string &error) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (state == nullptr || ::VirtualQuery(state, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        memory.Protect == PAGE_NOACCESS || (memory.Protect & PAGE_GUARD) != 0) {
        error = "permanent gateway state page is not committed readable memory";
        return false;
    }
    if (state->magic != kStateMagic || state->abi_version != kGatewayAbiVersion ||
        state->struct_size != sizeof(PermanentGatewayState) || state->entry != entry || state->gateway != gateway ||
        state->original == nullptr || state->trampoline == nullptr || state->original != state->trampoline) {
        error = "permanent gateway state signature/ABI validation failed";
        return false;
    }
    if (header.arity != static_cast<std::uint32_t>(arity)) {
        error = "permanent gateway arity does not match requested target ABI";
        return false;
    }
    const std::uint64_t gate = gateRead(state);
    if (!stateClosed(gate) || stateCount(gate) != 0 || handlerRead(state) != nullptr) {
        error = "rediscovered permanent gateway is not detached and drained; fail closed";
        return false;
    }
    return true;
}

bool tryRediscover(void *entry, PermanentGatewayArity arity, PermanentGateway &result, bool &owned_entry,
                   std::string &error)
{
    owned_entry = false;
    if (!isAlignedForAtomic8(reinterpret_cast<std::uintptr_t>(entry))) {
        error = "stable entry is not 8-byte aligned";
        return false;
    }

    std::array<std::uint8_t, 16> zeros{};
    const AtomicCompareResult observed = atomicCompareExchange8(entry, zeros, zeros);
    if (observed.observed[0] != 0xE9) {
        return true;
    }
    owned_entry = true;

    std::int32_t displacement = 0;
    std::memcpy(&displacement, observed.observed.data() + 1, sizeof(displacement));
    const std::uintptr_t entry_address = reinterpret_cast<std::uintptr_t>(entry);
    const std::uintptr_t gateway_address = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(entry_address + 5) + static_cast<std::intptr_t>(displacement));
    if (gateway_address < kGatewayOffset) {
        error = "entry rel32 target cannot contain a permanent gateway header";
        return false;
    }
    auto *gateway = reinterpret_cast<std::uint8_t *>(gateway_address);
    auto *header = reinterpret_cast<PermanentGatewayHeader *>(gateway_address - kGatewayOffset);

    MEMORY_BASIC_INFORMATION code_memory{};
    if (::VirtualQuery(header, &code_memory, sizeof(code_memory)) == 0 || code_memory.State != MEM_COMMIT ||
        !executableProtection(code_memory.Protect) || code_memory.AllocationBase == nullptr ||
        reinterpret_cast<std::uintptr_t>(header) < reinterpret_cast<std::uintptr_t>(code_memory.BaseAddress)) {
        error = "entry rel32 target is not a validated permanent gateway RX page";
        return false;
    }
    PermanentGatewayHeader snapshot{};
    std::memcpy(&snapshot, header, sizeof(snapshot));
    if (snapshot.magic != kHeaderMagic || snapshot.abi_version != kGatewayAbiVersion ||
        snapshot.header_size != sizeof(PermanentGatewayHeader) || snapshot.gateway_offset != kGatewayOffset ||
        snapshot.gateway_size == 0 || snapshot.gateway_size > kGatewayCapacity || snapshot.entry != entry ||
        snapshot.state == nullptr) {
        error = "entry rel32 owner is not the expected permanent gateway signature/ABI";
        return false;
    }

    auto *state = static_cast<PermanentGatewayState *>(snapshot.state);
    if (!validatePermanentState(state, entry, gateway, arity, snapshot, error)) {
        return false;
    }

    std::array<std::uint8_t, kGatewayCapacity> expected_code{};
    std::size_t expected_size = 0;
    if (!buildGatewayCode(arity, state, expected_code, expected_size, error) || expected_size != snapshot.gateway_size ||
        std::memcmp(gateway, expected_code.data(), expected_size) != 0 ||
        fnv1a(std::span<const std::uint8_t>(expected_code.data(), expected_size)) != snapshot.code_hash) {
        error = "permanent gateway executable bytes/hash validation failed";
        return false;
    }

    std::array<std::uint8_t, 16> original{};
    std::memcpy(original.data(), snapshot.original.data(), snapshot.original.size());
    std::array<std::uint8_t, 16> installed{};
    if (!encodeAtomic8RelayEntry(entry_address, gateway_address, original, installed, error) ||
        std::memcmp(installed.data(), observed.observed.data(), kAtomicEntryWidth8) != 0) {
        error = "stable entry bytes no longer match the permanent gateway owner";
        return false;
    }

    result.entry_ = entry;
    result.gateway_ = gateway;
    result.state_ = state;
    result.original_trampoline_ = state->trampoline;
    result.arity_ = arity;
    result.footprint_.island_reserved = static_cast<std::size_t>(state->island_reserved);
    result.footprint_.island_committed = static_cast<std::size_t>(state->island_committed);
    result.footprint_.trampoline_reserved_committed = static_cast<std::size_t>(state->trampoline_allocation);
    return true;
}

bool openGate(PermanentGatewayState *state) noexcept
{
    for (;;) {
        const std::uint64_t current = gateRead(state);
        if (!stateClosed(current) || stateCount(current) != 0 || stateGeneration(current) == kMaxEpoch) {
            return false;
        }
        const std::uint64_t desired = current & ~kClosedBit;
        const LONG64 observed = ::InterlockedCompareExchange64(&state->gate, static_cast<LONG64>(desired),
                                                               static_cast<LONG64>(current));
        if (static_cast<std::uint64_t>(observed) == current) {
            return true;
        }
    }
}

bool closeGate(PermanentGatewayState *state) noexcept
{
    for (;;) {
        const std::uint64_t current = gateRead(state);
        const std::uint64_t generation = current >> kEpochShift;
        if (stateClosed(current)) {
            return generation != kMaxEpoch;
        }
        const bool exhausted = generation == kMaxEpoch;
        const std::uint64_t next_generation = exhausted ? generation : generation + 1;
        const std::uint64_t desired = (next_generation << kEpochShift) | kClosedBit | stateCount(current);
        const LONG64 observed = ::InterlockedCompareExchange64(&state->gate, static_cast<LONG64>(desired),
                                                               static_cast<LONG64>(current));
        if (static_cast<std::uint64_t>(observed) == current) {
            return !exhausted;
        }
    }
}

bool pointerInRange(const void *pointer, std::uintptr_t begin, std::uintptr_t end) noexcept
{
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer);
    return pointer != nullptr && begin < end && value >= begin && value < end;
}

}  // namespace

bool PermanentGateway::installOrRediscover(void *entry, PermanentGatewayArity arity, PermanentGateway &result,
                                           std::string &error)
{
    error.clear();
    result = {};
    if (entry == nullptr) {
        error = "permanent gateway entry is null";
        return false;
    }
    if (!dynamicCodeAllowed(error)) {
        return false;
    }

    bool owned_entry = false;
    if (!tryRediscover(entry, arity, result, owned_entry, error)) {
        return false;
    }
    if (result.valid()) {
        return true;
    }
    if (owned_entry) {
        if (error.empty()) {
            error = "stable entry is already owned by an unrecognized rel32 gateway";
        }
        return false;
    }

    MEMORY_BASIC_INFORMATION entry_memory{};
    if (::VirtualQuery(entry, &entry_memory, sizeof(entry_memory)) == 0 || entry_memory.State != MEM_COMMIT) {
        error = "VirtualQuery stable entry failed: " + std::to_string(::GetLastError());
        return false;
    }
    const std::uintptr_t entry_address = reinterpret_cast<std::uintptr_t>(entry);
    const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(entry_memory.BaseAddress) + entry_memory.RegionSize;
    if (region_end < entry_address || region_end - entry_address < 16) {
        error = "stable entry does not expose a bounded 16-byte readable window";
        return false;
    }

    std::array<std::uint8_t, 16> original{};
    std::memcpy(original.data(), entry, original.size());

    BoundedRelocation relocation;
    if (!prepareBoundedRelocation(entry, relocation, error)) {
        return false;
    }
    // The atomic publication owns/checks eight bytes. Reject a relocation which
    // depends on bytes outside that ownership window rather than racing a third
    // party over copied instructions which our CAS cannot validate.
    if (relocation.patch_length > kAtomicEntryWidth8) {
        releaseBoundedRelocation(relocation);
        error = "bounded relocation exceeds the atomically owned 8-byte entry window";
        return false;
    }

    IslandBuild island;
    if (!reserveIslandNear(entry, arity, relocation, original, island, error)) {
        releaseBoundedRelocation(relocation);
        return false;
    }

    bool published = false;
    if (!publishEntry(entry, original, island.gateway, published, error)) {
        if (!published) {
            releaseUnpublishedIsland(island);
            releaseBoundedRelocation(relocation);
        }
        // Once publication succeeds, both allocations are intentionally leaked
        // process-lifetime even if a later protection/cache operation reports a
        // failure. Reclaiming them would reintroduce the stale-RIP hazard.
        return false;
    }

    result.entry_ = entry;
    result.gateway_ = island.gateway;
    result.state_ = island.state;
    result.original_trampoline_ = relocation.entry;
    result.arity_ = arity;
    result.footprint_.island_reserved = kIslandReservationSize;
    result.footprint_.island_committed = island.page_size * 2;
    result.footprint_.trampoline_reserved_committed = relocation.allocation_size;
    return true;
}

bool PermanentGateway::attach(void *new_handler, std::string &error) noexcept
{
    error.clear();
    if (!valid() || new_handler == nullptr) {
        error = "permanent gateway attach received invalid state/handler";
        return false;
    }
    auto *state = static_cast<PermanentGatewayState *>(state_);
    const std::uint64_t gate = gateRead(state);
    if (!stateClosed(gate) || stateCount(gate) != 0 || handlerRead(state) != nullptr) {
        error = "permanent gateway must be detached and drained before attach";
        return false;
    }

    (void)::InterlockedExchangePointer(&state->handler, new_handler);
    if (!openGate(state)) {
        (void)::InterlockedExchangePointer(&state->handler, nullptr);
        error = stateGeneration(gateRead(state)) == kMaxEpoch
                    ? "permanent gateway generation exhausted; admission remains permanently closed"
                    : "permanent gateway admission could not be opened";
        return false;
    }
    return true;
}

bool PermanentGateway::detach(std::uint64_t timeout_ms, std::string &error) noexcept
{
    error.clear();
    if (!valid()) {
        error = "permanent gateway detach received invalid state";
        return false;
    }
    auto *state = static_cast<PermanentGatewayState *>(state_);
    const bool generation_ok = closeGate(state);
    const std::uint64_t deadline = ::GetTickCount64() + timeout_ms;
    while (stateCount(gateRead(state)) != 0) {
        if (::GetTickCount64() >= deadline) {
            error = "timed out draining permanent gateway callbacks; handler remains published";
            return false;
        }
        ::Sleep(1);
    }

    (void)::InterlockedExchangePointer(&state->handler, nullptr);
    const std::uint64_t final_state = gateRead(state);
    if (!stateClosed(final_state) || stateCount(final_state) != 0 || handlerRead(state) != nullptr) {
        error = "permanent gateway detach postcondition failed";
        return false;
    }
    if (!generation_ok) {
        error = "permanent gateway generation exhausted; safely detached but can never reopen";
        return false;
    }
    return true;
}

bool PermanentGateway::admissionClosed() const noexcept
{
    return valid() && stateClosed(gateRead(static_cast<const PermanentGatewayState *>(state_)));
}

bool PermanentGateway::drained() const noexcept
{
    if (!valid()) {
        return false;
    }
    const std::uint64_t gate = gateRead(static_cast<const PermanentGatewayState *>(state_));
    return stateClosed(gate) && stateCount(gate) == 0;
}

std::uint32_t PermanentGateway::activeCount() const noexcept
{
    return valid() ? stateCount(gateRead(static_cast<const PermanentGatewayState *>(state_))) : 0;
}

std::uint32_t PermanentGateway::generation() const noexcept
{
    return valid() ? stateGeneration(gateRead(static_cast<const PermanentGatewayState *>(state_))) : 0;
}

void *PermanentGateway::handler() const noexcept
{
    return valid() ? handlerRead(static_cast<PermanentGatewayState *>(state_)) : nullptr;
}

bool PermanentGateway::containsAddressInRange(std::uintptr_t begin, std::uintptr_t end) const noexcept
{
    if (!valid()) {
        return false;
    }
    auto *state = static_cast<PermanentGatewayState *>(state_);
    return pointerInRange(handlerRead(state), begin, end) || pointerInRange(state->original, begin, end) ||
           pointerInRange(state->entry, begin, end) || pointerInRange(state->gateway, begin, end) ||
           pointerInRange(state->trampoline, begin, end) || pointerInRange(state_, begin, end) ||
           pointerInRange(gateway_, begin, end);
}

}  // namespace spark::stable_entry_experiment
