#include "native/alloc/windows_permanent_iat_gateway_experiment.h"

#ifndef _WIN32
#error "windows_permanent_iat_gateway_experiment.cpp must only be compiled on Windows"
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
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace spark::permanent_iat_gateway_experiment {
namespace {

constexpr std::uint64_t kGatewayMagic = 0x3154414947504B53ULL;  // "SKPGIAT1" marker.
constexpr std::uint32_t kGatewayAbiVersion = 1;
constexpr std::size_t kGatewayCodeCapacity = 128;
constexpr std::size_t kGatewayAllocationSize = 4096;
constexpr std::uint64_t kGateClosed = 0;
constexpr std::uint64_t kGateOpen = 1;
constexpr std::uint32_t kMaxStackArguments = 1;

struct GatewayState {
    std::uint64_t magic = kGatewayMagic;
    std::uint32_t abi_version = kGatewayAbiVersion;
    std::uint32_t struct_size = sizeof(GatewayState);
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> gate{kGateClosed};
    std::atomic<std::uint64_t> active{0};
    std::atomic<void *> handler{nullptr};
    void *original = nullptr;
    void *gateway = nullptr;
    std::uint64_t code_hash = 0;
    std::uint32_t code_size = 0;
    std::uint32_t stack_argument_count = 0;
};

static_assert(offsetof(GatewayState, generation) == 16);
static_assert(offsetof(GatewayState, gate) == 24);
static_assert(offsetof(GatewayState, active) == 32);
static_assert(offsetof(GatewayState, handler) == 40);
static_assert(offsetof(GatewayState, original) == 48);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<void *>::is_always_lock_free);

[[nodiscard]] std::uint64_t hashBytes(const void *data, std::size_t size) noexcept
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

[[nodiscard]] bool mitigationAllowsPrototype(std::string &error) noexcept
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamic_code{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &dynamic_code,
                                     static_cast<SIZE_T>(sizeof(dynamic_code))) == FALSE) {
        error = "GetProcessMitigationPolicy(ProcessDynamicCodePolicy) failed: " + std::to_string(::GetLastError());
        return false;
    }
    if (dynamic_code.ProhibitDynamicCode != 0) {
        error = "ProcessDynamicCodePolicy prohibits permanent IAT gateway executable code";
        return false;
    }

    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg,
                                     static_cast<SIZE_T>(sizeof(cfg))) == FALSE) {
        error = "GetProcessMitigationPolicy(ProcessControlFlowGuardPolicy) failed: " + std::to_string(::GetLastError());
        return false;
    }
    // Keep the same conservative policy as the entry-gateway prototype. The
    // generated gateway uses raw indirect CALL/JMP instructions; production CFG
    // support must be explicit and documented rather than silently assumed.
    if (cfg.EnableControlFlowGuard != 0) {
        error = "ProcessControlFlowGuardPolicy is enabled; raw permanent IAT gateway dispatch fails closed";
        return false;
    }
    return true;
}

[[nodiscard]] bool emit(std::array<std::uint8_t, kGatewayCodeCapacity> &code, std::size_t &out,
                        std::initializer_list<std::uint8_t> bytes, std::string &error)
{
    if (out + bytes.size() > code.size()) {
        error = "permanent IAT gateway machine-code buffer capacity exceeded";
        return false;
    }
    for (const std::uint8_t byte : bytes) {
        code[out++] = byte;
    }
    return true;
}

[[nodiscard]] bool patchRel8(std::array<std::uint8_t, kGatewayCodeCapacity> &code, std::size_t branch_offset,
                             std::size_t target, std::string &error)
{
    if (branch_offset + 2 > code.size() || target > code.size()) {
        error = "permanent IAT gateway branch patch is outside code buffer";
        return false;
    }
    const std::intptr_t delta = static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(branch_offset + 2);
    if (delta < -128 || delta > 127) {
        error = "permanent IAT gateway branch exceeds rel8 range";
        return false;
    }
    code[branch_offset + 1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(delta));
    return true;
}

[[nodiscard]] bool buildGatewayCode(GatewayState *state, std::array<std::uint8_t, kGatewayCodeCapacity> &code,
                                    std::size_t &code_size, std::string &error)
{
    code = {};
    code_size = 0;
    if (state == nullptr || state->stack_argument_count > kMaxStackArguments) {
        error = "unsupported permanent IAT gateway stack-argument count";
        return false;
    }

    // This is intentionally the same admission ABI as Permanent Gateway v2.
    // Before publication the code is unreachable. Once an IAT slot points at
    // it, no executable byte is modified again. The only publication operation
    // required by the caller is an atomic pointer-slot exchange.
    const std::uint64_t state_address = reinterpret_cast<std::uint64_t>(state);
    if (!emit(code, code_size, {0x49, 0xBB}, error)) {  // mov r11,state
        return false;
    }
    if (code_size + sizeof(state_address) > code.size()) {
        error = "permanent IAT gateway state immediate exceeds code buffer";
        return false;
    }
    std::memcpy(code.data() + code_size, &state_address, sizeof(state_address));
    code_size += sizeof(state_address);

    if (!emit(code, code_size, {0x49, 0x83, 0x7B, 0x18, 0x00}, error)) {  // cmp [r11+gate],0
        return false;
    }
    const std::size_t initial_fallback = code_size;
    if (!emit(code, code_size, {0x74, 0x00}, error) ||                    // je fallback
        !emit(code, code_size, {0x4D, 0x8B, 0x53, 0x10}, error) ||        // mov r10,[r11+generation]
        !emit(code, code_size, {0xF0, 0x49, 0xFF, 0x43, 0x20}, error) ||  // lock inc [r11+active]
        !emit(code, code_size, {0x49, 0x83, 0x7B, 0x18, 0x00}, error)) {  // cmp [r11+gate],0
        return false;
    }
    const std::size_t closed_rollback = code_size;
    if (!emit(code, code_size, {0x74, 0x00}, error) ||              // je rollback
        !emit(code, code_size, {0x4D, 0x3B, 0x53, 0x10}, error)) {  // cmp r10,[r11+generation]
        return false;
    }
    const std::size_t generation_rollback = code_size;
    if (!emit(code, code_size, {0x75, 0x00}, error) ||              // jne rollback
        !emit(code, code_size, {0x4D, 0x8B, 0x53, 0x28}, error) ||  // mov r10,[r11+handler]
        !emit(code, code_size, {0x4D, 0x85, 0xD2}, error)) {        // test r10,r10
        return false;
    }
    const std::size_t null_rollback = code_size;
    if (!emit(code, code_size, {0x74, 0x00}, error)) {  // je rollback
        return false;
    }

    if (state->stack_argument_count == 1 &&
        !emit(code, code_size, {0x4C, 0x8B, 0x5C, 0x24, 0x28}, error)) {  // mov r11,[rsp+40]
        return false;
    }
    if (!emit(code, code_size, {0x48, 0x83, 0xEC, 0x28}, error)) {  // sub rsp,40
        return false;
    }
    if (state->stack_argument_count == 1 &&
        !emit(code, code_size, {0x4C, 0x89, 0x5C, 0x24, 0x20}, error)) {  // mov [rsp+32],r11
        return false;
    }
    if (!emit(code, code_size, {0x41, 0xFF, 0xD2}, error) ||        // call r10
        !emit(code, code_size, {0x48, 0x83, 0xC4, 0x28}, error) ||  // add rsp,40
        !emit(code, code_size, {0x49, 0xBB}, error)) {              // mov r11,state
        return false;
    }
    if (code_size + sizeof(state_address) > code.size()) {
        error = "permanent IAT gateway post-call state immediate exceeds code buffer";
        return false;
    }
    std::memcpy(code.data() + code_size, &state_address, sizeof(state_address));
    code_size += sizeof(state_address);
    if (!emit(code, code_size, {0xF0, 0x49, 0xFF, 0x4B, 0x20}, error) ||  // lock dec [r11+active]
        !emit(code, code_size, {0xC3}, error)) {                          // ret original caller
        return false;
    }

    const std::size_t rollback = code_size;
    if (!emit(code, code_size, {0xF0, 0x49, 0xFF, 0x4B, 0x20}, error)) {  // lock dec [r11+active]
        return false;
    }
    const std::size_t fallback = code_size;
    if (!emit(code, code_size, {0x4D, 0x8B, 0x53, 0x30}, error) ||  // mov r10,[r11+original]
        !emit(code, code_size, {0x41, 0xFF, 0xE2}, error)) {        // jmp r10
        return false;
    }

    return patchRel8(code, initial_fallback, fallback, error) && patchRel8(code, closed_rollback, rollback, error) &&
           patchRel8(code, generation_rollback, rollback, error) && patchRel8(code, null_rollback, rollback, error);
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
    if (address == nullptr || ::VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        memory.BaseAddress == nullptr || memory.RegionSize == 0) {
        error = "permanent IAT gateway address is not committed memory";
        return false;
    }
    return true;
}

[[nodiscard]] GatewayState *stateFromHandle(const PermanentIatGatewayHandle &handle) noexcept
{
    return static_cast<GatewayState *>(handle.state);
}

[[nodiscard]] bool validateStateIdentity(GatewayState *state, void *gateway, std::string &error)
{
    if (state == nullptr || state->magic != kGatewayMagic || state->abi_version != kGatewayAbiVersion ||
        state->struct_size != sizeof(GatewayState) || state->gateway != gateway || state->original == nullptr ||
        state->stack_argument_count > kMaxStackArguments || state->code_size == 0 ||
        state->code_size > kGatewayCodeCapacity) {
        error = "permanent IAT gateway state identity/ABI validation failed";
        return false;
    }
    return true;
}

[[nodiscard]] bool waitForZero(GatewayState *state, std::uint64_t timeout_ms, std::string &error)
{
    const std::uint64_t deadline = ::GetTickCount64() + timeout_ms;
    while (state->active.load(std::memory_order_acquire) != 0) {
        if (::GetTickCount64() >= deadline) {
            error = "timed out draining permanent IAT gateway admitted callbacks";
            return false;
        }
        ::Sleep(0);
    }
    return true;
}

void populateHandle(GatewayState *state, const MEMORY_BASIC_INFORMATION &code_memory,
                    const MEMORY_BASIC_INFORMATION &state_memory, PermanentIatGatewayHandle &handle) noexcept
{
    handle.gateway = state->gateway;
    handle.original = state->original;
    handle.state = state;
    handle.permanent_rx_bytes = code_memory.RegionSize;
    handle.permanent_rw_bytes = state_memory.RegionSize;
    handle.generation = state->generation.load(std::memory_order_acquire);
    handle.stack_argument_count = state->stack_argument_count;
}

}  // namespace

bool createPermanentIatGateway(void *original, std::uint32_t stack_argument_count, PermanentIatGatewayHandle &handle,
                               std::string &error)
{
    handle = {};
    error.clear();
    if (original == nullptr) {
        error = "permanent IAT gateway original is null";
        return false;
    }
    if (stack_argument_count > kMaxStackArguments) {
        error = "permanent IAT gateway prototype supports at most one stack argument";
        return false;
    }
    if (!mitigationAllowsPrototype(error)) {
        return false;
    }

    MEMORY_BASIC_INFORMATION original_memory{};
    if (!queryCommitted(original, original_memory, error) || !isExecutableReadOnly(original_memory.Protect)) {
        error = "permanent IAT gateway original is not committed executable read-only memory";
        return false;
    }

    void *state_memory_raw = ::VirtualAlloc(nullptr, sizeof(GatewayState), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (state_memory_raw == nullptr) {
        error = "VirtualAlloc permanent IAT gateway state failed: " + std::to_string(::GetLastError());
        return false;
    }
    void *code_memory_raw = ::VirtualAlloc(nullptr, kGatewayAllocationSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (code_memory_raw == nullptr) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "VirtualAlloc permanent IAT gateway code failed: " + std::to_string(failure);
        return false;
    }

    auto *state = ::new (state_memory_raw) GatewayState{};
    state->original = original;
    state->gateway = code_memory_raw;
    state->stack_argument_count = stack_argument_count;

    std::array<std::uint8_t, kGatewayCodeCapacity> code{};
    std::size_t code_size = 0;
    if (!buildGatewayCode(state, code, code_size, error)) {
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        return false;
    }
    state->code_size = static_cast<std::uint32_t>(code_size);
    std::memcpy(code_memory_raw, code.data(), code_size);

    DWORD old_code_protection = 0;
    if (::VirtualProtect(code_memory_raw, kGatewayAllocationSize, PAGE_EXECUTE_READ, &old_code_protection) == FALSE) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "VirtualProtect permanent IAT gateway RX failed: " + std::to_string(failure);
        return false;
    }
    if (::FlushInstructionCache(::GetCurrentProcess(), code_memory_raw, kGatewayAllocationSize) == FALSE) {
        const DWORD failure = ::GetLastError();
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "FlushInstructionCache permanent IAT gateway failed: " + std::to_string(failure);
        return false;
    }
    state->code_hash = hashBytes(code_memory_raw, code_size);

    MEMORY_BASIC_INFORMATION code_memory{};
    MEMORY_BASIC_INFORMATION state_memory{};
    if (!queryCommitted(code_memory_raw, code_memory, error) || !isExecutableReadOnly(code_memory.Protect) ||
        !queryCommitted(state, state_memory, error) || !isWritableNonExecutable(state_memory.Protect)) {
        ::VirtualFree(code_memory_raw, 0, MEM_RELEASE);
        ::VirtualFree(state_memory_raw, 0, MEM_RELEASE);
        error = "permanent IAT gateway memory protections violate W^X invariant before publication";
        return false;
    }

    // This is the lifetime boundary for successful construction. The caller may
    // publish gateway into arbitrary process IAT slots immediately after return,
    // so successful code/state is process-lifetime and has no destroy API.
    populateHandle(state, code_memory, state_memory, handle);
    return true;
}

bool discoverPermanentIatGateway(void *gateway, PermanentIatGatewayHandle &handle, std::string &error)
{
    handle = {};
    error.clear();
    if (gateway == nullptr) {
        error = "permanent IAT gateway discovery pointer is null";
        return false;
    }

    MEMORY_BASIC_INFORMATION code_memory{};
    if (!queryCommitted(gateway, code_memory, error) || !isExecutableReadOnly(code_memory.Protect)) {
        error = "permanent IAT gateway discovery pointer is not committed RX memory";
        return false;
    }
    const auto gateway_value = reinterpret_cast<std::uintptr_t>(gateway);
    const auto code_region_end = reinterpret_cast<std::uintptr_t>(code_memory.BaseAddress) + code_memory.RegionSize;
    auto *code = static_cast<std::uint8_t *>(gateway);
    if (gateway_value > code_region_end || code_region_end - gateway_value < 10 || code[0] != 0x49 || code[1] != 0xBB) {
        error = "permanent IAT gateway does not contain the expected state signature";
        return false;
    }

    std::uint64_t state_value = 0;
    std::memcpy(&state_value, code + 2, sizeof(state_value));
    auto *state = reinterpret_cast<GatewayState *>(state_value);
    MEMORY_BASIC_INFORMATION state_memory{};
    if (!queryCommitted(state, state_memory, error) || !isWritableNonExecutable(state_memory.Protect)) {
        error = "decoded permanent IAT gateway state is not committed non-executable writable memory";
        return false;
    }
    if (!validateStateIdentity(state, gateway, error)) {
        return false;
    }

    std::array<std::uint8_t, kGatewayCodeCapacity> expected_code{};
    std::size_t expected_size = 0;
    if (!buildGatewayCode(state, expected_code, expected_size, error) || expected_size != state->code_size ||
        std::memcmp(gateway, expected_code.data(), expected_size) != 0 ||
        state->code_hash != hashBytes(gateway, expected_size)) {
        if (error.empty()) {
            error = "permanent IAT gateway code signature/hash validation failed";
        }
        return false;
    }

    MEMORY_BASIC_INFORMATION original_memory{};
    if (!queryCommitted(state->original, original_memory, error) || !isExecutableReadOnly(original_memory.Protect)) {
        error = "permanent IAT gateway original is no longer committed RX memory";
        return false;
    }

    populateHandle(state, code_memory, state_memory, handle);
    return true;
}

bool bindPermanentIatGateway(PermanentIatGatewayHandle &handle, void *handler, std::uint64_t timeout_ms,
                             std::string &error)
{
    error.clear();
    auto *state = stateFromHandle(handle);
    if (state == nullptr || handler == nullptr || !validateStateIdentity(state, handle.gateway, error)) {
        if (error.empty()) {
            error = "permanent IAT gateway bind received a null state or handler";
        }
        return false;
    }

    MEMORY_BASIC_INFORMATION handler_memory{};
    if (!queryCommitted(handler, handler_memory, error) || !isExecutableReadOnly(handler_memory.Protect)) {
        error = "permanent IAT gateway handler is not committed executable read-only memory";
        return false;
    }
    if (state->gate.load(std::memory_order_acquire) != kGateClosed ||
        state->handler.load(std::memory_order_acquire) != nullptr) {
        error = "permanent IAT gateway bind requires a detached state";
        return false;
    }
    if (!waitForZero(state, timeout_ms, error)) {
        return false;
    }

    const std::uint64_t current = state->generation.load(std::memory_order_acquire);
    if (current >= (std::numeric_limits<std::uint64_t>::max)() - 1) {
        error = "permanent IAT gateway generation exhausted; admission remains closed";
        return false;
    }
    const std::uint64_t generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    state->handler.store(handler, std::memory_order_release);
    state->gate.store(kGateOpen, std::memory_order_seq_cst);
    handle.generation = generation;
    return true;
}

bool detachPermanentIatGateway(PermanentIatGatewayHandle &handle, std::uint64_t timeout_ms, std::string &error)
{
    error.clear();
    auto *state = stateFromHandle(handle);
    if (state == nullptr || !validateStateIdentity(state, handle.gateway, error)) {
        if (error.empty()) {
            error = "permanent IAT gateway detach received a null state";
        }
        return false;
    }

    state->gate.store(kGateClosed, std::memory_order_seq_cst);
    const std::uint64_t current = state->generation.load(std::memory_order_acquire);
    if (current == (std::numeric_limits<std::uint64_t>::max)()) {
        error = "permanent IAT gateway generation exhausted after admission close";
        return false;
    }
    const std::uint64_t generation = state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    handle.generation = generation;

    if (!waitForZero(state, timeout_ms, error)) {
        return false;
    }
    state->handler.store(nullptr, std::memory_order_seq_cst);
    return true;
}

void *permanentIatGatewayOriginal(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->original : nullptr;
}

void *permanentIatGatewayHandler(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->handler.load(std::memory_order_acquire) : nullptr;
}

std::uint64_t permanentIatGatewayActive(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->active.load(std::memory_order_acquire) : 0;
}

std::uint64_t permanentIatGatewayGeneration(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr ? state->generation.load(std::memory_order_acquire) : 0;
}

bool permanentIatGatewayAdmissionOpen(const PermanentIatGatewayHandle &handle) noexcept
{
    GatewayState *state = stateFromHandle(handle);
    return state != nullptr && state->gate.load(std::memory_order_acquire) == kGateOpen;
}

}  // namespace spark::permanent_iat_gateway_experiment
