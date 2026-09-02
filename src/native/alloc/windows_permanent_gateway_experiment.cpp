#include "native/alloc/windows_permanent_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_experiment.cpp is Windows-only"
#endif

#include <array>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <funchook.h>

#include "native/alloc/windows_stable_entry_atomic.h"

namespace spark::permanent_gateway_experiment {
namespace {

constexpr std::uint64_t kIslandMagic = 0x3157444757505353ULL;  // "SSPWGDW1" opaque ABI marker
constexpr std::uint64_t kStateMagic = 0x3154455441545350ULL;
constexpr std::size_t kUnwindInfoOffset = 512;
constexpr std::size_t kMaxGatewayCodeSize = 384;
constexpr std::uint64_t kCountMask = 0x00000000FFFFFFFFULL;
constexpr std::uint64_t kClosedBit = 0x0000000100000000ULL;
constexpr unsigned kEpochShift = 33;
constexpr std::uint64_t kMaxEpoch = 0x7FFFFFFFULL;

struct GatewayIslandHeader {
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t abi = 0;
    std::uint64_t state_address = 0;
    std::uint64_t entry_address = 0;
    std::uint64_t code_size = 0;
    std::uint64_t code_hash = 0;
    std::uint32_t stack_argument_count = 0;
    std::uint32_t reserved = 0;
    std::uint64_t reserved2 = 0;
};
static_assert(sizeof(GatewayIslandHeader) == kGatewayCodeOffset);

struct alignas(64) GatewayState {
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t abi = 0;
    volatile LONG64 gate_state = static_cast<LONG64>(kClosedBit);
    PVOID volatile handler = nullptr;
    PVOID original = nullptr;
    volatile LONG64 owner_cookie = 0;
    volatile LONG64 attach_generation = 0;
    std::uint32_t stack_argument_count = 0;
    std::uint32_t runtime_registered = 0;
    RUNTIME_FUNCTION runtime_function{};
    std::uint32_t reserved_runtime = 0;
    std::uint64_t entry_address = 0;
};
static_assert(offsetof(GatewayState, gate_state) == 16);
static_assert(offsetof(GatewayState, handler) == 24);
static_assert(offsetof(GatewayState, original) == 32);

struct CodeBuilder {
    std::array<std::uint8_t, kMaxGatewayCodeSize> bytes{};
    std::size_t size = 0;

    void emit(std::uint8_t value) noexcept
    {
        assert(size < bytes.size());
        bytes[size++] = value;
    }

    void emit32(std::uint32_t value) noexcept
    {
        assert(size + sizeof(value) <= bytes.size());
        std::memcpy(bytes.data() + size, &value, sizeof(value));
        size += sizeof(value);
    }

    void emit64(std::uint64_t value) noexcept
    {
        assert(size + sizeof(value) <= bytes.size());
        std::memcpy(bytes.data() + size, &value, sizeof(value));
        size += sizeof(value);
    }

    void bytesN(std::initializer_list<std::uint8_t> values) noexcept
    {
        for (std::uint8_t value : values) {
            emit(value);
        }
    }

    [[nodiscard]] std::size_t jump32() noexcept
    {
        emit(0xE9);
        const std::size_t displacement = size;
        emit32(0);
        return displacement;
    }

    [[nodiscard]] std::size_t conditional32(std::uint8_t opcode) noexcept
    {
        bytesN({0x0F, opcode});
        const std::size_t displacement = size;
        emit32(0);
        return displacement;
    }

    void patch32(std::size_t displacement_offset, std::size_t target) noexcept
    {
        const std::int64_t delta = static_cast<std::int64_t>(target) -
                                   static_cast<std::int64_t>(displacement_offset + sizeof(std::uint32_t));
        assert(delta >= INT32_MIN && delta <= INT32_MAX);
        const std::int32_t encoded = static_cast<std::int32_t>(delta);
        std::memcpy(bytes.data() + displacement_offset, &encoded, sizeof(encoded));
    }
};

struct BuiltGatewayCode {
    CodeBuilder builder;
    std::uint8_t prolog_size = 0;
};

[[nodiscard]] std::uint64_t fnv1a64(const void *data, std::size_t size) noexcept
{
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] bool rel32Reachable(std::uintptr_t instruction_end, std::uintptr_t destination) noexcept
{
    if (destination >= instruction_end) {
        return destination - instruction_end <= static_cast<std::uintptr_t>(INT32_MAX);
    }
    return instruction_end - destination <= static_cast<std::uintptr_t>(INT32_MAX) + 1ULL;
}

[[nodiscard]] LONG64 load64(volatile LONG64 *value) noexcept
{
    return ::InterlockedCompareExchange64(value, 0, 0);
}

[[nodiscard]] PVOID loadPointer(PVOID volatile *value) noexcept
{
    return ::InterlockedCompareExchangePointer(value, nullptr, nullptr);
}

[[nodiscard]] std::uint32_t countOf(std::uint64_t state) noexcept
{
    return static_cast<std::uint32_t>(state & kCountMask);
}

[[nodiscard]] bool closedOf(std::uint64_t state) noexcept
{
    return (state & kClosedBit) != 0;
}

[[nodiscard]] std::uint64_t epochOf(std::uint64_t state) noexcept
{
    return state >> kEpochShift;
}

[[nodiscard]] bool openGate(GatewayState *state) noexcept
{
    LONG64 current_signed = load64(&state->gate_state);
    for (;;) {
        const std::uint64_t current = static_cast<std::uint64_t>(current_signed);
        if (!closedOf(current) || countOf(current) != 0 || epochOf(current) == kMaxEpoch) {
            return false;
        }
        const LONG64 desired = static_cast<LONG64>(current & ~kClosedBit);
        const LONG64 observed = ::InterlockedCompareExchange64(&state->gate_state, desired, current_signed);
        if (observed == current_signed) {
            return true;
        }
        current_signed = observed;
    }
}

[[nodiscard]] bool closeGate(GatewayState *state) noexcept
{
    LONG64 current_signed = load64(&state->gate_state);
    for (;;) {
        const std::uint64_t current = static_cast<std::uint64_t>(current_signed);
        if (closedOf(current)) {
            return epochOf(current) != kMaxEpoch;
        }
        const std::uint64_t epoch = epochOf(current);
        const bool exhausted = epoch == kMaxEpoch;
        const std::uint64_t next_epoch = exhausted ? epoch : epoch + 1;
        const std::uint64_t desired_value =
            (next_epoch << kEpochShift) | kClosedBit | static_cast<std::uint64_t>(countOf(current));
        const LONG64 desired = static_cast<LONG64>(desired_value);
        const LONG64 observed = ::InterlockedCompareExchange64(&state->gate_state, desired, current_signed);
        if (observed == current_signed) {
            return !exhausted;
        }
        current_signed = observed;
    }
}

[[nodiscard]] bool mitigationsAllowGateway(std::string &error) noexcept
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamic{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &dynamic, sizeof(dynamic)) ==
        FALSE) {
        error = "GetProcessMitigationPolicy(ProcessDynamicCodePolicy) failed";
        return false;
    }
    if (dynamic.ProhibitDynamicCode != 0) {
        error = "ProcessDynamicCodePolicy prohibits permanent gateway code generation";
        return false;
    }

    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg, sizeof(cfg)) == FALSE) {
        error = "GetProcessMitigationPolicy(ProcessControlFlowGuardPolicy) failed";
        return false;
    }
    // This first production-oriented prototype deliberately fails closed rather
    // than emitting an indirect handler call that bypasses CFG dispatch checks.
    if (cfg.EnableControlFlowGuard != 0) {
        error = "CFG-enabled processes are not supported by the current permanent gateway ABI";
        return false;
    }
    return true;
}

[[nodiscard]] void *reserveNearIsland(void *entry, std::size_t &committed_size, std::string &error) noexcept
{
    SYSTEM_INFO system{};
    ::GetSystemInfo(&system);
    const std::uintptr_t granularity = system.dwAllocationGranularity != 0 ? system.dwAllocationGranularity : 64 * 1024;
    const std::size_t page_size = system.dwPageSize != 0 ? system.dwPageSize : 4096;
    const std::uintptr_t entry_address = reinterpret_cast<std::uintptr_t>(entry);
    const std::uintptr_t base = entry_address & ~(granularity - 1);
    const std::uintptr_t max_distance = static_cast<std::uintptr_t>(INT32_MAX) - granularity;

    for (std::uintptr_t distance = 0; distance <= max_distance; distance += granularity) {
        const std::uintptr_t candidates[2] = {
            base >= distance ? base - distance : 0,
            base <= (std::numeric_limits<std::uintptr_t>::max)() - distance ? base + distance : 0,
        };
        for (std::uintptr_t candidate : candidates) {
            if (candidate == 0 || !rel32Reachable(entry_address + 5, candidate + kGatewayCodeOffset)) {
                continue;
            }
            void *reservation =
                ::VirtualAlloc(reinterpret_cast<void *>(candidate), granularity, MEM_RESERVE, PAGE_NOACCESS);
            if (reservation == nullptr || reinterpret_cast<std::uintptr_t>(reservation) != candidate) {
                if (reservation != nullptr) {
                    ::VirtualFree(reservation, 0, MEM_RELEASE);
                }
                continue;
            }
            void *committed = ::VirtualAlloc(reservation, page_size, MEM_COMMIT, PAGE_READWRITE);
            if (committed != reservation) {
                ::VirtualFree(reservation, 0, MEM_RELEASE);
                continue;
            }
            committed_size = page_size;
            return reservation;
        }
        if (max_distance - distance < granularity) {
            break;
        }
    }
    error = "could not reserve a rel32-reachable permanent gateway island";
    return nullptr;
}

[[nodiscard]] bool preparePermanentTrampoline(void *entry, void *&trampoline, funchook_t *&relocator,
                                              std::string &error) noexcept
{
    relocator = funchook_create();
    if (relocator == nullptr) {
        error = "funchook_create failed while preparing permanent original trampoline";
        return false;
    }
    void *callable = entry;
    const int code = funchook_prepare(relocator, &callable, entry);
    if (code != FUNCHOOK_ERROR_SUCCESS || callable == nullptr || callable == entry) {
        const char *detail = funchook_error_message(relocator);
        char buffer[256]{};
        std::snprintf(buffer, sizeof(buffer), "permanent trampoline relocation failed (code=%d): %s", code,
                      detail != nullptr ? detail : "no detail");
        error = buffer;
        (void)funchook_destroy(relocator);
        relocator = nullptr;
        return false;
    }

    MEMORY_BASIC_INFORMATION memory{};
    if (::VirtualQuery(callable, &memory, sizeof(memory)) == 0 || memory.BaseAddress == nullptr ||
        memory.RegionSize == 0 || memory.State != MEM_COMMIT) {
        error = "VirtualQuery permanent original trampoline failed";
        (void)funchook_destroy(relocator);
        relocator = nullptr;
        return false;
    }
    DWORD old = 0;
    if (::VirtualProtect(memory.BaseAddress, memory.RegionSize, PAGE_EXECUTE_READ, &old) == FALSE) {
        error = "VirtualProtect permanent original trampoline RX failed";
        (void)funchook_destroy(relocator);
        relocator = nullptr;
        return false;
    }
    if (::FlushInstructionCache(::GetCurrentProcess(), memory.BaseAddress, memory.RegionSize) == FALSE) {
        error = "FlushInstructionCache permanent original trampoline failed";
        (void)funchook_destroy(relocator);
        relocator = nullptr;
        return false;
    }
    trampoline = callable;
    return true;
}

[[nodiscard]] BuiltGatewayCode buildGatewayCode(GatewayState *state, std::uint32_t stack_argument_count) noexcept
{
    BuiltGatewayCode built;
    CodeBuilder &code = built.builder;

    // Keep the pass-through block physically before the stack-allocation point.
    // The dynamic unwind record can therefore describe one fixed 40-byte stack
    // allocation for every RIP after the generated prologue without lying about
    // the closed-gate path.
    const std::size_t entry_jump = code.jump32();
    const std::size_t passthrough = code.size;
    code.bytesN({0x49, 0xBA});  // mov r10, imm64
    code.emit64(reinterpret_cast<std::uint64_t>(state));
    code.bytesN({0x4D, 0x8B, 0x5A, 0x20});  // mov r11, [r10+32] (original)
    code.bytesN({0x41, 0xFF, 0xE3});        // jmp r11

    const std::size_t gate_check = code.size;
    code.patch32(entry_jump, gate_check);
    code.bytesN({0x49, 0xBA});  // mov r10, imm64
    code.emit64(reinterpret_cast<std::uint64_t>(state));

    const std::size_t retry = code.size;
    code.bytesN({0x49, 0x8B, 0x42, 0x10});        // mov rax, [r10+16]
    code.bytesN({0x48, 0x0F, 0xBA, 0xE0, 0x20});  // bt rax, 32
    const std::size_t closed_jump = code.conditional32(0x82);  // jc passthrough
    code.bytesN({0x83, 0xF8, 0xFF});                          // cmp eax, -1
    const std::size_t overflow_jump = code.conditional32(0x84);  // je passthrough
    code.bytesN({0x49, 0x89, 0xC3});                             // mov r11, rax
    code.bytesN({0x49, 0x83, 0xC3, 0x01});                       // add r11, 1
    code.bytesN({0xF0, 0x4D, 0x0F, 0xB1, 0x5A, 0x10});           // lock cmpxchg [r10+16], r11
    const std::size_t retry_jump = code.conditional32(0x85);     // jne retry
    code.patch32(closed_jump, passthrough);
    code.patch32(overflow_jump, passthrough);
    code.patch32(retry_jump, retry);

    if (stack_argument_count == 1) {
        code.bytesN({0x48, 0x8B, 0x44, 0x24, 0x28});  // mov rax, [rsp+40]
    }
    code.bytesN({0x48, 0x83, 0xEC, 0x28});  // sub rsp, 40
    built.prolog_size = static_cast<std::uint8_t>(code.size);
    if (stack_argument_count == 1) {
        code.bytesN({0x48, 0x89, 0x44, 0x24, 0x20});  // mov [rsp+32], rax
    }
    code.bytesN({0x4D, 0x8B, 0x5A, 0x18});  // mov r11, [r10+24] (handler)
    code.bytesN({0x41, 0xFF, 0xD3});        // call r11
    code.bytesN({0x49, 0x89, 0xC3});        // mov r11, rax (save return)
    code.bytesN({0x49, 0xBA});              // mov r10, imm64
    code.emit64(reinterpret_cast<std::uint64_t>(state));

    const std::size_t leave_retry = code.size;
    code.bytesN({0x49, 0x8B, 0x42, 0x10});              // mov rax, [r10+16]
    code.bytesN({0x48, 0x89, 0xC2});                    // mov rdx, rax
    code.bytesN({0x48, 0x83, 0xEA, 0x01});              // sub rdx, 1
    code.bytesN({0xF0, 0x49, 0x0F, 0xB1, 0x52, 0x10});  // lock cmpxchg [r10+16], rdx
    const std::size_t leave_jump = code.conditional32(0x85);  // jne leave_retry
    code.patch32(leave_jump, leave_retry);
    code.bytesN({0x4C, 0x89, 0xD8});        // mov rax, r11
    code.bytesN({0x48, 0x83, 0xC4, 0x28});  // add rsp, 40
    code.emit(0xC3);                        // ret
    return built;
}

void writeUnwindInfo(std::uint8_t *island, std::uint8_t prolog_size) noexcept
{
    // UNWIND_INFO: version 1, no flags, one UWOP_ALLOC_SMALL entry for 40 bytes.
    // OpInfo=(40/8)-1=4, UnwindOp=2 => 0x42.
    std::array<std::uint8_t, 8> unwind{};
    unwind[0] = 0x01;
    unwind[1] = prolog_size;
    unwind[2] = 0x01;
    unwind[3] = 0x00;
    unwind[4] = prolog_size;
    unwind[5] = 0x42;
    std::memcpy(island + kUnwindInfoOffset, unwind.data(), unwind.size());
}

[[nodiscard]] bool validateProtection(void *address, bool executable, bool writable, MEMORY_BASIC_INFORMATION &memory,
                                      std::string &error) noexcept
{
    if (::VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT) {
        error = "permanent gateway memory is not committed";
        return false;
    }
    const DWORD protection = memory.Protect & 0xFFU;
    const bool is_executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                               protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const bool is_writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                             protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    if (is_executable != executable || is_writable != writable) {
        error = "permanent gateway memory protection does not match RX/RW ABI";
        return false;
    }
    return true;
}

[[nodiscard]] bool rediscoverExisting(void *entry, std::uint32_t stack_argument_count, PermanentGateway &gateway,
                                      bool &found, std::string &error) noexcept
{
    found = false;
    std::array<std::uint8_t, 8> entry_bytes{};
    std::memcpy(entry_bytes.data(), entry, entry_bytes.size());
    if (entry_bytes[0] != 0xE9) {
        return true;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, entry_bytes.data() + 1, sizeof(displacement));
    const std::uintptr_t gateway_address = reinterpret_cast<std::uintptr_t>(entry) + 5 + displacement;
    if (gateway_address < kGatewayCodeOffset) {
        return true;
    }
    const std::uintptr_t island_address = gateway_address - kGatewayCodeOffset;

    MEMORY_BASIC_INFORMATION island_memory{};
    if (::VirtualQuery(reinterpret_cast<void *>(island_address), &island_memory, sizeof(island_memory)) == 0 ||
        island_memory.State != MEM_COMMIT) {
        return true;
    }
    const DWORD island_protection = island_memory.Protect & 0xFFU;
    if (island_protection != PAGE_EXECUTE_READ) {
        return true;
    }

    GatewayIslandHeader header{};
    std::memcpy(&header, reinterpret_cast<const void *>(island_address), sizeof(header));
    if (header.magic != kIslandMagic || header.version != kGatewayAbiVersion || header.abi != kGatewayAbiVersion ||
        header.entry_address != reinterpret_cast<std::uint64_t>(entry) ||
        header.stack_argument_count != stack_argument_count || header.code_size == 0 ||
        header.code_size > kMaxGatewayCodeSize || header.state_address == 0) {
        return true;
    }
    if (fnv1a64(reinterpret_cast<const void *>(gateway_address), static_cast<std::size_t>(header.code_size)) !=
        header.code_hash) {
        error = "permanent gateway code hash mismatch";
        return false;
    }

    auto *state = reinterpret_cast<GatewayState *>(header.state_address);
    MEMORY_BASIC_INFORMATION state_memory{};
    if (!validateProtection(state, false, true, state_memory, error)) {
        return false;
    }
    if (state->magic != kStateMagic || state->version != kGatewayAbiVersion || state->abi != kGatewayAbiVersion ||
        state->entry_address != reinterpret_cast<std::uint64_t>(entry) ||
        state->stack_argument_count != stack_argument_count || state->original == nullptr ||
        state->runtime_registered == 0) {
        error = "permanent gateway RW state validation failed";
        return false;
    }

    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = ::RtlLookupFunctionEntry(static_cast<DWORD64>(gateway_address), &image_base, nullptr);
    if (function == nullptr || image_base != static_cast<DWORD64>(island_address) ||
        function->BeginAddress != kGatewayCodeOffset || function->EndAddress != kGatewayCodeOffset + header.code_size ||
        function->UnwindData != kUnwindInfoOffset) {
        error = "permanent gateway runtime unwind registration is missing or inconsistent";
        return false;
    }

    gateway.entry_ = entry;
    gateway.island_ = reinterpret_cast<void *>(island_address);
    gateway.state_ = state;
    gateway.code_size_ = static_cast<std::size_t>(header.code_size);
    found = true;
    return true;
}

[[nodiscard]] std::size_t committedRegionSize(void *address) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (address == nullptr || ::VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT) {
        return 0;
    }
    return memory.RegionSize;
}

}  // namespace

bool PermanentGateway::installOrRediscover(void *entry, std::uint32_t stack_argument_count, PermanentGateway &gateway,
                                           bool &created, std::string &error)
{
    created = false;
    error.clear();
    gateway = PermanentGateway{};
    if (entry == nullptr || stack_argument_count > 1) {
        error = "invalid permanent gateway entry or stack-argument ABI";
        return false;
    }

    bool found = false;
    if (!rediscoverExisting(entry, stack_argument_count, gateway, found, error)) {
        return false;
    }
    if (found) {
        return true;
    }
    if (!mitigationsAllowGateway(error)) {
        return false;
    }
    const std::uintptr_t entry_address = reinterpret_cast<std::uintptr_t>(entry);
    if (!stable_entry_experiment::isAlignedForAtomic8(entry_address)) {
        error = "stable allocator entry is not 8-byte aligned";
        return false;
    }

    MEMORY_BASIC_INFORMATION entry_memory{};
    if (::VirtualQuery(entry, &entry_memory, sizeof(entry_memory)) == 0 || entry_memory.State != MEM_COMMIT) {
        error = "VirtualQuery stable allocator entry failed";
        return false;
    }
    const std::uintptr_t entry_region_end =
        reinterpret_cast<std::uintptr_t>(entry_memory.BaseAddress) + entry_memory.RegionSize;
    if (entry_address > entry_region_end || entry_region_end - entry_address < 16) {
        error = "stable allocator entry does not expose a bounded 16-byte window";
        return false;
    }
    std::array<std::uint8_t, 16> original_bytes{};
    std::memcpy(original_bytes.data(), entry, original_bytes.size());

    void *trampoline = nullptr;
    funchook_t *relocator = nullptr;
    if (!preparePermanentTrampoline(entry, trampoline, relocator, error)) {
        return false;
    }

    SYSTEM_INFO system{};
    ::GetSystemInfo(&system);
    const std::size_t page_size = system.dwPageSize != 0 ? system.dwPageSize : 4096;
    auto *state = static_cast<GatewayState *>(
        ::VirtualAlloc(nullptr, page_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (state == nullptr) {
        error = "VirtualAlloc permanent gateway state failed";
        (void)funchook_destroy(relocator);
        return false;
    }
    state->magic = kStateMagic;
    state->version = kGatewayAbiVersion;
    state->abi = kGatewayAbiVersion;
    state->gate_state = static_cast<LONG64>(kClosedBit);
    state->handler = nullptr;
    state->original = trampoline;
    state->owner_cookie = 0;
    state->attach_generation = 0;
    state->stack_argument_count = stack_argument_count;
    state->runtime_registered = 0;
    state->entry_address = entry_address;

    std::size_t island_committed = 0;
    auto *island = static_cast<std::uint8_t *>(reserveNearIsland(entry, island_committed, error));
    if (island == nullptr) {
        ::VirtualFree(state, 0, MEM_RELEASE);
        (void)funchook_destroy(relocator);
        return false;
    }
    if (island_committed < kUnwindInfoOffset + 8) {
        error = "permanent gateway island page is too small for code and unwind metadata";
        ::VirtualFree(island, 0, MEM_RELEASE);
        ::VirtualFree(state, 0, MEM_RELEASE);
        (void)funchook_destroy(relocator);
        return false;
    }

    BuiltGatewayCode built = buildGatewayCode(state, stack_argument_count);
    if (built.builder.size == 0 || built.builder.size > kMaxGatewayCodeSize || built.prolog_size == 0) {
        error = "generated permanent gateway code is outside ABI bounds";
        ::VirtualFree(island, 0, MEM_RELEASE);
        ::VirtualFree(state, 0, MEM_RELEASE);
        (void)funchook_destroy(relocator);
        return false;
    }

    GatewayIslandHeader header{};
    header.magic = kIslandMagic;
    header.version = kGatewayAbiVersion;
    header.abi = kGatewayAbiVersion;
    header.state_address = reinterpret_cast<std::uint64_t>(state);
    header.entry_address = entry_address;
    header.code_size = built.builder.size;
    header.code_hash = fnv1a64(built.builder.bytes.data(), built.builder.size);
    header.stack_argument_count = stack_argument_count;
    std::memcpy(island, &header, sizeof(header));
    std::memcpy(island + kGatewayCodeOffset, built.builder.bytes.data(), built.builder.size);
    writeUnwindInfo(island, built.prolog_size);

    state->runtime_function.BeginAddress = static_cast<DWORD>(kGatewayCodeOffset);
    state->runtime_function.EndAddress = static_cast<DWORD>(kGatewayCodeOffset + built.builder.size);
    state->runtime_function.UnwindData = static_cast<DWORD>(kUnwindInfoOffset);

    DWORD old_island_protection = 0;
    if (::VirtualProtect(island, island_committed, PAGE_EXECUTE_READ, &old_island_protection) == FALSE ||
        ::FlushInstructionCache(::GetCurrentProcess(), island, island_committed) == FALSE) {
        error = "publishing permanent gateway island RX failed";
        ::VirtualFree(island, 0, MEM_RELEASE);
        ::VirtualFree(state, 0, MEM_RELEASE);
        (void)funchook_destroy(relocator);
        return false;
    }
    if (::RtlAddFunctionTable(&state->runtime_function, 1, reinterpret_cast<DWORD64>(island)) == FALSE) {
        error = "RtlAddFunctionTable permanent gateway registration failed";
        ::VirtualFree(island, 0, MEM_RELEASE);
        ::VirtualFree(state, 0, MEM_RELEASE);
        (void)funchook_destroy(relocator);
        return false;
    }
    state->runtime_registered = 1;

    std::array<std::uint8_t, 16> installed{};
    if (!stable_entry_experiment::encodeAtomic8RelayEntry(
            entry_address, reinterpret_cast<std::uintptr_t>(island + kGatewayCodeOffset), original_bytes, installed,
            error)) {
        (void)::RtlDeleteFunctionTable(&state->runtime_function);
        ::VirtualFree(island, 0, MEM_RELEASE);
        ::VirtualFree(state, 0, MEM_RELEASE);
        (void)funchook_destroy(relocator);
        return false;
    }

    DWORD old_entry_protection = 0;
    if (::VirtualProtect(entry, stable_entry_experiment::kAtomicEntryWidth8, PAGE_EXECUTE_READWRITE,
                         &old_entry_protection) == FALSE) {
        error = "VirtualProtect before permanent entry publication failed";
        (void)::RtlDeleteFunctionTable(&state->runtime_function);
        ::VirtualFree(island, 0, MEM_RELEASE);
        ::VirtualFree(state, 0, MEM_RELEASE);
        (void)funchook_destroy(relocator);
        return false;
    }
    const stable_entry_experiment::AtomicCompareResult exchanged =
        stable_entry_experiment::atomicCompareExchange8(entry, original_bytes, installed);
    if (!exchanged.exchanged) {
        DWORD ignored = 0;
        (void)::VirtualProtect(entry, stable_entry_experiment::kAtomicEntryWidth8, old_entry_protection, &ignored);
        error = "stable allocator entry ownership changed before permanent publication";
        (void)::RtlDeleteFunctionTable(&state->runtime_function);
        ::VirtualFree(island, 0, MEM_RELEASE);
        ::VirtualFree(state, 0, MEM_RELEASE);
        (void)funchook_destroy(relocator);
        return false;
    }

    // From this point on the island, trampoline, state and function table are
    // process-lifetime resources. Never reclaim them on Spark unload. Even if a
    // later protection/flush operation fails, the closed gate contains no Spark
    // handler pointer and both public execution paths remain valid.
    bool publication_ok = true;
    if (::FlushInstructionCache(::GetCurrentProcess(), entry, stable_entry_experiment::kAtomicEntryWidth8) == FALSE) {
        error = "FlushInstructionCache after permanent entry publication failed";
        publication_ok = false;
    }
    DWORD ignored = 0;
    if (::VirtualProtect(entry, stable_entry_experiment::kAtomicEntryWidth8, old_entry_protection, &ignored) == FALSE) {
        if (error.empty()) {
            error = "VirtualProtect restore after permanent entry publication failed";
        }
        publication_ok = false;
    }

    // Intentionally abandon relocator ownership. funchook's prepared executable
    // trampoline and its bookkeeping allocations are now process-lifetime and
    // are not required to execute any Spark code after this install call returns.
    (void)relocator;

    gateway.entry_ = entry;
    gateway.island_ = island;
    gateway.state_ = state;
    gateway.code_size_ = built.builder.size;
    created = true;
    return publication_ok;
}

bool PermanentGateway::attach(void *handler, std::uint64_t owner_cookie, std::string &error) noexcept
{
    error.clear();
    if (!valid() || handler == nullptr || owner_cookie == 0) {
        error = "invalid permanent gateway attach request";
        return false;
    }
    auto *state = static_cast<GatewayState *>(state_);
    if (!drained() || handlerAddress() != nullptr) {
        error = "permanent gateway is not closed and drained before attach";
        return false;
    }
    const LONG64 cookie = static_cast<LONG64>(owner_cookie);
    if (::InterlockedCompareExchange64(&state->owner_cookie, cookie, 0) != 0) {
        error = "permanent gateway is already owned by another handler generation";
        return false;
    }
    (void)::InterlockedExchangePointer(&state->handler, handler);
    if (!openGate(state)) {
        (void)::InterlockedExchangePointer(&state->handler, nullptr);
        (void)::InterlockedCompareExchange64(&state->owner_cookie, 0, cookie);
        error = "permanent gateway admission gate could not be opened";
        return false;
    }
    (void)::InterlockedIncrement64(&state->attach_generation);
    return true;
}

bool PermanentGateway::detach(std::uint64_t owner_cookie, std::uint64_t timeout_ms, std::string &error) noexcept
{
    error.clear();
    if (!valid() || owner_cookie == 0) {
        error = "invalid permanent gateway detach request";
        return false;
    }
    auto *state = static_cast<GatewayState *>(state_);
    const LONG64 cookie = static_cast<LONG64>(owner_cookie);
    if (load64(&state->owner_cookie) != cookie) {
        error = "permanent gateway detach owner cookie mismatch";
        return false;
    }

    const bool reusable = closeGate(state);
    const std::uint64_t deadline = ::GetTickCount64() + timeout_ms;
    while (!drained()) {
        if (::GetTickCount64() >= deadline) {
            error = "timed out draining permanent gateway callbacks";
            return false;
        }
        ::Sleep(1);
    }

    (void)::InterlockedExchangePointer(&state->handler, nullptr);
    if (::InterlockedCompareExchange64(&state->owner_cookie, 0, cookie) != cookie) {
        error = "permanent gateway owner changed while finalizing detach";
        return false;
    }
    if (!reusable) {
        error = "permanent gateway safely detached but admission epoch is exhausted";
        return false;
    }
    return true;
}

bool PermanentGateway::drained() const noexcept
{
    if (!valid()) {
        return false;
    }
    const auto *state = static_cast<const GatewayState *>(state_);
    const std::uint64_t gate = static_cast<std::uint64_t>(load64(const_cast<volatile LONG64 *>(&state->gate_state)));
    return closedOf(gate) && countOf(gate) == 0;
}

std::uint32_t PermanentGateway::activeCount() const noexcept
{
    if (!valid()) {
        return 0;
    }
    const auto *state = static_cast<const GatewayState *>(state_);
    return countOf(static_cast<std::uint64_t>(load64(const_cast<volatile LONG64 *>(&state->gate_state))));
}

std::uint32_t PermanentGateway::generation() const noexcept
{
    if (!valid()) {
        return 0;
    }
    const auto *state = static_cast<const GatewayState *>(state_);
    return static_cast<std::uint32_t>(
        epochOf(static_cast<std::uint64_t>(load64(const_cast<volatile LONG64 *>(&state->gate_state)))));
}

void *PermanentGateway::handlerAddress() const noexcept
{
    if (!valid()) {
        return nullptr;
    }
    auto *state = static_cast<GatewayState *>(state_);
    return loadPointer(&state->handler);
}

void *PermanentGateway::originalTrampoline() const noexcept
{
    if (!valid()) {
        return nullptr;
    }
    return static_cast<GatewayState *>(state_)->original;
}

void *PermanentGateway::gatewayEntry() const noexcept
{
    return island_ != nullptr ? static_cast<std::uint8_t *>(island_) + kGatewayCodeOffset : nullptr;
}

GatewayFootprint PermanentGateway::footprint() const noexcept
{
    GatewayFootprint result;
    if (!valid()) {
        return result;
    }
    result.island_committed = committedRegionSize(island_);
    result.state_committed = committedRegionSize(state_);
    result.trampoline_committed = committedRegionSize(originalTrampoline());
    return result;
}

}  // namespace spark::permanent_gateway_experiment
