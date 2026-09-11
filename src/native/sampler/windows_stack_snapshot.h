#ifndef ENDSTONE_SPARK_WINDOWS_STACK_SNAPSHOT_H
#define ENDSTONE_SPARK_WINDOWS_STACK_SNAPSHOT_H

#ifndef _WIN32
#error "windows_stack_snapshot.h is Windows-only"
#endif

#if !defined(_M_X64) && !defined(__x86_64__)
#error "windows_stack_snapshot.h supports Windows x64 only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace spark {

struct WindowsStackSnapshot {
    static constexpr std::size_t kMaxBytes = 128U * 1024U;

    [[nodiscard]] bool setRange(std::uintptr_t base, std::size_t size) noexcept
    {
        if (base == 0 || size > kMaxBytes || base > (std::numeric_limits<std::uintptr_t>::max)() - size) {
            return false;
        }
        base_ = base;
        size_ = size;
        return true;
    }

    void clear() noexcept
    {
        base_ = 0;
        size_ = 0;
    }

    [[nodiscard]] std::uintptr_t base() const noexcept { return base_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint8_t *data() noexcept { return bytes_.data(); }
    [[nodiscard]] const std::uint8_t *data() const noexcept { return bytes_.data(); }

    [[nodiscard]] bool contains(std::uintptr_t address, std::size_t bytes) const noexcept
    {
        if (base_ == 0 || address < base_ || bytes > size_) {
            return false;
        }
        const auto offset = address - base_;
        return offset <= size_ - bytes;
    }

    [[nodiscard]] bool read(std::uintptr_t address, void *destination, std::size_t bytes) const noexcept
    {
        if (destination == nullptr || !contains(address, bytes)) {
            return false;
        }
        std::memcpy(destination, bytes_.data() + (address - base_), bytes);
        return true;
    }

    template <typename T>
    [[nodiscard]] bool read(std::uintptr_t address, T &value) const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return read(address, &value, sizeof(T));
    }

private:
    alignas(16) std::array<std::uint8_t, kMaxBytes> bytes_{};
    std::uintptr_t base_ = 0;
    std::size_t size_ = 0;
};

enum class WindowsWalkStatus {
    Frame,
    Complete,
    Failure,
};

inline constexpr std::size_t kWindowsStackSnapshotRegionQueryLimit = 32;
inline constexpr std::size_t kWindowsStackUnwindStepLimit = 256;
inline constexpr std::size_t kWindowsStackUnwindChainLimit = 16;
inline constexpr std::size_t kWindowsStackEpilogueByteLimit = 64;

[[nodiscard]] inline bool windowsCanonicalAddress(std::uintptr_t address) noexcept
{
    const auto upper = static_cast<std::uint64_t>(address) >> 47U;
    return upper == 0 || upper == 0x1ffffU;
}

[[nodiscard]] inline bool windowsAddAddress(std::uintptr_t address, std::uint64_t amount,
                                            std::uintptr_t &result) noexcept
{
    if (amount > (std::numeric_limits<std::uintptr_t>::max)() - address) {
        return false;
    }
    result = address + static_cast<std::uintptr_t>(amount);
    return true;
}

[[nodiscard]] inline bool windowsSubtractAddress(std::uintptr_t address, std::uint64_t amount,
                                                 std::uintptr_t &result) noexcept
{
    if (amount > address) {
        return false;
    }
    result = address - static_cast<std::uintptr_t>(amount);
    return true;
}

struct WindowsRuntimeFunction {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t unwind_info = 0;
    std::uintptr_t image_base = 0;
};

enum class WindowsFunctionLookupStatus {
    Leaf,
    Function,
    Failure,
};

using WindowsFunctionLookup = WindowsFunctionLookupStatus (*)(std::uintptr_t control_pc,
                                                              WindowsRuntimeFunction &function, void *context) noexcept;
using WindowsMetadataReader = bool (*)(std::uintptr_t address, void *destination, std::size_t bytes,
                                       void *context) noexcept;

namespace windows_stack_snapshot_detail {

constexpr std::uint8_t KUnwindVersion = 1;
constexpr std::uint8_t KUnwindFlagEHandler = 1U;
constexpr std::uint8_t KUnwindFlagUHandler = 2U;
constexpr std::uint8_t KUnwindFlagChainInfo = 4U;
constexpr std::size_t KMaxUnwindCodeSlots = 255;

enum class UnwindOp : std::uint8_t {
    PushNonVol = 0,
    AllocLarge = 1,
    AllocSmall = 2,
    SetFpReg = 3,
    SaveNonVol = 4,
    SaveNonVolFar = 5,
    SaveXmm128 = 8,
    SaveXmm128Far = 9,
    PushMachFrame = 10,
};

struct ParsedUnwindInfo {
    std::uint8_t version = 0;
    std::uint8_t flags = 0;
    std::uint8_t prolog_size = 0;
    std::uint8_t code_count = 0;
    std::uint8_t frame_register = 0;
    std::uint8_t frame_offset = 0;
    std::array<std::uint8_t, KMaxUnwindCodeSlots * 2> code_bytes{};
    bool has_chain = false;
    WindowsRuntimeFunction chain{};
};

struct ApplyState {
    std::array<std::uintptr_t, kWindowsStackUnwindChainLimit> visited_unwind_infos{};
    std::size_t visited_count = 0;
    std::uint8_t frame_register = 0;
    std::uint8_t frame_offset = 0;
    std::uintptr_t frame_base = 0;
    bool frame_info_set = false;
    bool frame_base_set = false;
    bool machine_frame = false;
};

[[nodiscard]] inline bool readMetadata(WindowsMetadataReader reader, void *reader_context, std::uintptr_t address,
                                       void *destination, std::size_t bytes) noexcept
{
    return reader != nullptr && address != 0 && destination != nullptr && bytes != 0 &&
           reader(address, destination, bytes, reader_context);
}

[[nodiscard]] inline bool validNonVolatileRegister(std::uint8_t reg) noexcept
{
    return reg == 3 || reg == 5 || reg == 6 || reg == 7 || (reg >= 12 && reg <= 15);
}

[[nodiscard]] inline bool validXmmRegister(std::uint8_t reg) noexcept
{
    return reg >= 6 && reg <= 15;
}

[[nodiscard]] inline DWORD64 *integerRegister(CONTEXT &context, std::uint8_t reg) noexcept
{
    switch (reg) {
    case 0:
        return &context.Rax;
    case 1:
        return &context.Rcx;
    case 2:
        return &context.Rdx;
    case 3:
        return &context.Rbx;
    case 4:
        return &context.Rsp;
    case 5:
        return &context.Rbp;
    case 6:
        return &context.Rsi;
    case 7:
        return &context.Rdi;
    case 8:
        return &context.R8;
    case 9:
        return &context.R9;
    case 10:
        return &context.R10;
    case 11:
        return &context.R11;
    case 12:
        return &context.R12;
    case 13:
        return &context.R13;
    case 14:
        return &context.R14;
    case 15:
        return &context.R15;
    default:
        return nullptr;
    }
}

[[nodiscard]] inline M128A *xmmRegister(CONTEXT &context, std::uint8_t reg) noexcept
{
    switch (reg) {
    case 0:
        return &context.Xmm0;
    case 1:
        return &context.Xmm1;
    case 2:
        return &context.Xmm2;
    case 3:
        return &context.Xmm3;
    case 4:
        return &context.Xmm4;
    case 5:
        return &context.Xmm5;
    case 6:
        return &context.Xmm6;
    case 7:
        return &context.Xmm7;
    case 8:
        return &context.Xmm8;
    case 9:
        return &context.Xmm9;
    case 10:
        return &context.Xmm10;
    case 11:
        return &context.Xmm11;
    case 12:
        return &context.Xmm12;
    case 13:
        return &context.Xmm13;
    case 14:
        return &context.Xmm14;
    case 15:
        return &context.Xmm15;
    default:
        return nullptr;
    }
}

[[nodiscard]] inline std::uint16_t readSlot(const ParsedUnwindInfo &info, std::size_t index) noexcept
{
    const auto offset = index * 2;
    return static_cast<std::uint16_t>(info.code_bytes[offset]) | static_cast<std::uint16_t>(info.code_bytes[offset + 1])
                                                                     << 8U;
}

[[nodiscard]] inline std::uint32_t readDword(const std::uint8_t *bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] inline bool parseUnwindInfo(const WindowsRuntimeFunction &function, ParsedUnwindInfo &info,
                                          WindowsMetadataReader reader, void *reader_context) noexcept
{
    if (function.unwind_info == 0) {
        return false;
    }

    std::array<std::uint8_t, 4> header{};
    if (!readMetadata(reader, reader_context, function.unwind_info, header.data(), header.size())) {
        return false;
    }
    info.version = header[0] & 0x07U;
    info.flags = static_cast<std::uint8_t>(header[0] >> 3U);
    info.prolog_size = header[1];
    info.code_count = header[2];
    info.frame_register = header[3] & 0x0fU;
    info.frame_offset = static_cast<std::uint8_t>(header[3] >> 4U);
    if (info.version != KUnwindVersion ||
        (info.flags & ~(KUnwindFlagEHandler | KUnwindFlagUHandler | KUnwindFlagChainInfo)) != 0 ||
        info.code_count > KMaxUnwindCodeSlots ||
        (info.flags & KUnwindFlagChainInfo) != 0 && (info.flags & (KUnwindFlagEHandler | KUnwindFlagUHandler)) != 0 ||
        info.frame_register != 0 && !validNonVolatileRegister(info.frame_register)) {
        return false;
    }
    if (info.frame_register == 0 && info.frame_offset != 0) {
        return false;
    }

    const auto code_bytes = static_cast<std::size_t>(info.code_count) * 2U;
    if (function.unwind_info > (std::numeric_limits<std::uintptr_t>::max)() - 4U ||
        (code_bytes != 0 &&
         (code_bytes > (std::numeric_limits<std::uintptr_t>::max)() - function.unwind_info - 4U ||
          !readMetadata(reader, reader_context, function.unwind_info + 4U, info.code_bytes.data(), code_bytes)))) {
        return false;
    }

    std::uint8_t previous_offset = 0xffU;
    for (std::size_t index = 0; index < info.code_count;) {
        const std::uint8_t code_offset = info.code_bytes[index * 2U];
        const std::uint8_t operation_byte = info.code_bytes[index * 2U + 1U];
        const auto operation = static_cast<UnwindOp>(operation_byte & 0x0fU);
        const std::uint8_t operation_info = operation_byte >> 4U;
        if (code_offset > info.prolog_size || code_offset > previous_offset) {
            return false;
        }
        previous_offset = code_offset;

        std::size_t slots = 1;
        switch (operation) {
        case UnwindOp::PushNonVol:
            if (!validNonVolatileRegister(operation_info)) {
                return false;
            }
            break;
        case UnwindOp::AllocLarge:
            if (operation_info == 0) {
                slots = 2;
            }
            else if (operation_info == 1) {
                slots = 3;
            }
            else {
                return false;
            }
            break;
        case UnwindOp::AllocSmall:
            break;
        case UnwindOp::SetFpReg:
            if (operation_info != 0 || info.frame_register == 0) {
                return false;
            }
            break;
        case UnwindOp::SaveNonVol:
            if (!validNonVolatileRegister(operation_info)) {
                return false;
            }
            slots = 2;
            break;
        case UnwindOp::SaveNonVolFar:
            if (!validNonVolatileRegister(operation_info)) {
                return false;
            }
            slots = 3;
            break;
        case UnwindOp::SaveXmm128:
            if (!validXmmRegister(operation_info)) {
                return false;
            }
            slots = 2;
            break;
        case UnwindOp::SaveXmm128Far:
            if (!validXmmRegister(operation_info)) {
                return false;
            }
            slots = 3;
            break;
        case UnwindOp::PushMachFrame:
            if (operation_info > 1) {
                return false;
            }
            break;
        default:
            return false;
        }
        if (index > info.code_count || slots > static_cast<std::size_t>(info.code_count) - index) {
            return false;
        }
        index += slots;
    }

    info.has_chain = (info.flags & KUnwindFlagChainInfo) != 0;
    if (info.has_chain) {
        if (function.image_base == 0) {
            return false;
        }
        const std::size_t padded_slots = (static_cast<std::size_t>(info.code_count) + 1U) & ~1U;
        const std::size_t chain_offset = 4U + padded_slots * 2U;
        if (function.unwind_info > (std::numeric_limits<std::uintptr_t>::max)() - chain_offset) {
            return false;
        }
        std::array<std::uint8_t, sizeof(DWORD) * 3U> chain_bytes{};
        if (!readMetadata(reader, reader_context, function.unwind_info + chain_offset, chain_bytes.data(),
                          chain_bytes.size())) {
            return false;
        }
        info.chain.begin = 0;
        info.chain.end = 0;
        info.chain.unwind_info = 0;
        info.chain.image_base = function.image_base;
        const auto begin_rva = readDword(chain_bytes.data());
        const auto end_rva = readDword(chain_bytes.data() + sizeof(DWORD));
        const auto unwind_rva = readDword(chain_bytes.data() + sizeof(DWORD) * 2U);
        if (begin_rva >= end_rva ||
            static_cast<std::uintptr_t>(begin_rva) >
                (std::numeric_limits<std::uintptr_t>::max)() - function.image_base ||
            static_cast<std::uintptr_t>(end_rva) > (std::numeric_limits<std::uintptr_t>::max)() - function.image_base ||
            static_cast<std::uintptr_t>(unwind_rva) >
                (std::numeric_limits<std::uintptr_t>::max)() - function.image_base) {
            return false;
        }
        info.chain.begin = function.image_base + begin_rva;
        info.chain.end = function.image_base + end_rva;
        info.chain.unwind_info = function.image_base + unwind_rva;
    }
    return true;
}

[[nodiscard]] inline bool stackRead(const WindowsStackSnapshot &snapshot, std::uintptr_t address, void *destination,
                                    std::size_t bytes) noexcept
{
    return snapshot.read(address, destination, bytes);
}

[[nodiscard]] inline bool readStackQword(const WindowsStackSnapshot &snapshot, std::uintptr_t address,
                                         DWORD64 &value) noexcept
{
    return stackRead(snapshot, address, &value, sizeof(value));
}

[[nodiscard]] inline bool ensureFrameBase(CONTEXT &context, ApplyState &state) noexcept
{
    if (state.frame_base_set) {
        return true;
    }
    if (state.frame_register == 0) {
        state.frame_base = static_cast<std::uintptr_t>(context.Rsp);
    }
    else {
        const auto *frame_register = integerRegister(context, state.frame_register);
        if (frame_register == nullptr ||
            !windowsSubtractAddress(static_cast<std::uintptr_t>(*frame_register),
                                    static_cast<std::uint64_t>(state.frame_offset) * 16U, state.frame_base)) {
            return false;
        }
    }
    state.frame_base_set = true;
    return true;
}

[[nodiscard]] inline bool applyUnwindInfo(const WindowsRuntimeFunction &function, const ParsedUnwindInfo &info,
                                          bool apply_all, CONTEXT &context, const WindowsStackSnapshot &snapshot,
                                          WindowsMetadataReader reader, void *reader_context,
                                          ApplyState &state) noexcept;

[[nodiscard]] inline bool applyUnwindCode(const ParsedUnwindInfo &info, std::size_t index, std::size_t slots,
                                          bool apply, CONTEXT &context, const WindowsStackSnapshot &snapshot,
                                          ApplyState &state) noexcept
{
    if (!apply) {
        return true;
    }

    const auto operation_byte = info.code_bytes[index * 2U + 1U];
    const auto operation = static_cast<UnwindOp>(operation_byte & 0x0fU);
    const std::uint8_t operation_info = operation_byte >> 4U;
    const auto rsp = static_cast<std::uintptr_t>(context.Rsp);
    switch (operation) {
    case UnwindOp::PushNonVol: {
        DWORD64 value = 0;
        if (!readStackQword(snapshot, rsp, value)) {
            return false;
        }
        auto *reg = integerRegister(context, operation_info);
        std::uintptr_t next_rsp = 0;
        if (reg == nullptr || !windowsAddAddress(rsp, sizeof(DWORD64), next_rsp)) {
            return false;
        }
        context.Rsp = static_cast<DWORD64>(next_rsp);
        *reg = value;
        return true;
    }
    case UnwindOp::AllocLarge: {
        std::uint64_t allocation = 0;
        if (operation_info == 0) {
            allocation = static_cast<std::uint64_t>(readSlot(info, index + 1U)) * sizeof(DWORD64);
        }
        else if (operation_info == 1) {
            allocation = static_cast<std::uint64_t>(readSlot(info, index + 1U)) |
                         (static_cast<std::uint64_t>(readSlot(info, index + 2U)) << 16U);
        }
        else {
            return false;
        }
        std::uintptr_t next_rsp = 0;
        if (allocation == 0 || !windowsAddAddress(rsp, allocation, next_rsp)) {
            return false;
        }
        context.Rsp = static_cast<DWORD64>(next_rsp);
        return true;
    }
    case UnwindOp::AllocSmall: {
        const auto allocation = static_cast<std::uint64_t>(operation_info + 1U) * sizeof(DWORD64);
        std::uintptr_t next_rsp = 0;
        if (!windowsAddAddress(rsp, allocation, next_rsp)) {
            return false;
        }
        context.Rsp = static_cast<DWORD64>(next_rsp);
        return true;
    }
    case UnwindOp::SetFpReg: {
        auto *reg = integerRegister(context, state.frame_register);
        if (reg == nullptr || !ensureFrameBase(context, state)) {
            return false;
        }
        context.Rsp = static_cast<DWORD64>(state.frame_base);
        return true;
    }
    case UnwindOp::SaveNonVol:
    case UnwindOp::SaveNonVolFar: {
        const std::uint64_t offset = operation == UnwindOp::SaveNonVol
                                       ? static_cast<std::uint64_t>(readSlot(info, index + 1U)) * sizeof(DWORD64)
                                       : static_cast<std::uint64_t>(readSlot(info, index + 1U)) |
                                             (static_cast<std::uint64_t>(readSlot(info, index + 2U)) << 16U);
        std::uintptr_t address = 0;
        DWORD64 value = 0;
        auto *reg = integerRegister(context, operation_info);
        if (reg == nullptr || !ensureFrameBase(context, state) ||
            !windowsAddAddress(state.frame_base, offset, address) || !readStackQword(snapshot, address, value)) {
            return false;
        }
        *reg = value;
        return true;
    }
    case UnwindOp::SaveXmm128:
    case UnwindOp::SaveXmm128Far: {
        const std::uint64_t offset = operation == UnwindOp::SaveXmm128
                                       ? static_cast<std::uint64_t>(readSlot(info, index + 1U)) * 16U
                                       : static_cast<std::uint64_t>(readSlot(info, index + 1U)) |
                                             (static_cast<std::uint64_t>(readSlot(info, index + 2U)) << 16U);
        std::uintptr_t address = 0;
        M128A value{};
        auto *reg = xmmRegister(context, operation_info);
        if (reg == nullptr || !ensureFrameBase(context, state) ||
            !windowsAddAddress(state.frame_base, offset, address) ||
            !stackRead(snapshot, address, &value, sizeof(value))) {
            return false;
        }
        *reg = value;
        return true;
    }
    case UnwindOp::PushMachFrame: {
        DWORD64 return_address = 0;
        const auto return_offset = operation_info == 0 ? 0U : sizeof(DWORD64);
        const auto frame_size = operation_info == 0 ? 40U : 48U;
        std::uintptr_t return_location = 0;
        std::uintptr_t next_rsp = 0;
        if (!windowsAddAddress(rsp, return_offset, return_location) ||
            !readStackQword(snapshot, return_location, return_address) ||
            !windowsAddAddress(rsp, frame_size, next_rsp)) {
            return false;
        }
        context.Rsp = static_cast<DWORD64>(next_rsp);
        context.Rip = return_address;
        state.machine_frame = true;
        return true;
    }
    default:
        (void)slots;
        return false;
    }
}

[[nodiscard]] inline bool applyUnwindInfo(const WindowsRuntimeFunction &function, const ParsedUnwindInfo &info,
                                          bool apply_all, CONTEXT &context, const WindowsStackSnapshot &snapshot,
                                          WindowsMetadataReader reader, void *reader_context,
                                          ApplyState &state) noexcept
{
    if (state.visited_count >= state.visited_unwind_infos.size()) {
        return false;
    }
    for (std::size_t index = 0; index < state.visited_count; ++index) {
        if (state.visited_unwind_infos[index] == function.unwind_info) {
            return false;
        }
    }
    state.visited_unwind_infos[state.visited_count++] = function.unwind_info;

    if (!state.frame_info_set) {
        state.frame_register = info.frame_register;
        state.frame_offset = info.frame_offset;
        state.frame_info_set = true;
    }
    else if (state.frame_register != info.frame_register || state.frame_offset != info.frame_offset) {
        return false;
    }

    const auto control_offset = function.begin <= static_cast<std::uintptr_t>(context.Rip)
                                  ? static_cast<std::uintptr_t>(context.Rip) - function.begin
                                  : (std::numeric_limits<std::uintptr_t>::max)();
    const bool in_prolog = !apply_all && control_offset <= info.prolog_size;
    for (std::size_t index = 0; index < info.code_count;) {
        const auto code_offset = info.code_bytes[index * 2U];
        const auto operation = static_cast<UnwindOp>(info.code_bytes[index * 2U + 1U] & 0x0fU);
        std::size_t slots = 1;
        if (operation == UnwindOp::AllocLarge) {
            slots = (info.code_bytes[index * 2U + 1U] >> 4U) == 0 ? 2U : 3U;
        }
        else if (operation == UnwindOp::SaveNonVol || operation == UnwindOp::SaveXmm128) {
            slots = 2;
        }
        else if (operation == UnwindOp::SaveNonVolFar || operation == UnwindOp::SaveXmm128Far) {
            slots = 3;
        }
        const bool apply = !in_prolog || code_offset <= control_offset;
        if (!applyUnwindCode(info, index, slots, apply, context, snapshot, state)) {
            return false;
        }
        index += slots;
    }

    if (!info.has_chain) {
        return true;
    }
    ParsedUnwindInfo chained{};
    if (!parseUnwindInfo(info.chain, chained, reader, reader_context)) {
        return false;
    }
    return applyUnwindInfo(info.chain, chained, true, context, snapshot, reader, reader_context, state);
}

enum class EpilogueResult {
    NotEpilogue,
    Unwound,
    Ambiguous,
};

[[nodiscard]] inline EpilogueResult unwindEpilogue(const WindowsRuntimeFunction &function, const ParsedUnwindInfo &info,
                                                   CONTEXT &context, const WindowsStackSnapshot &snapshot,
                                                   WindowsMetadataReader reader, void *reader_context) noexcept
{
    if (reader == nullptr || function.begin >= function.end || context.Rip < function.begin ||
        context.Rip >= function.end) {
        return EpilogueResult::NotEpilogue;
    }

    CONTEXT next = context;
    std::uintptr_t instruction = static_cast<std::uintptr_t>(next.Rip);
    const auto available = function.end - instruction;
    const auto budget = available < kWindowsStackEpilogueByteLimit ? available : kWindowsStackEpilogueByteLimit;
    const auto scan_end = instruction + budget;
    bool recognized = false;
    while (instruction < scan_end) {
        const auto read = [&](std::size_t offset, void *destination, std::size_t count) {
            const auto remaining = scan_end - instruction;
            return offset <= remaining && count <= remaining - offset &&
                   readMetadata(reader, reader_context, instruction + offset, destination, count);
        };
        std::uint8_t first = 0;
        if (!read(0, &first, sizeof(first))) {
            return recognized ? EpilogueResult::Ambiguous : EpilogueResult::NotEpilogue;
        }

        if (first == 0xc3U || first == 0xc2U) {
            std::array<std::uint8_t, 2> immediate{};
            if (first == 0xc2U && !read(1, immediate.data(), immediate.size())) {
                return EpilogueResult::Ambiguous;
            }
            DWORD64 return_address = 0;
            std::uintptr_t next_rsp = 0;
            const auto adjustment = sizeof(DWORD64) + static_cast<std::uint64_t>(immediate[0]) +
                                    (static_cast<std::uint64_t>(immediate[1]) << 8U);
            if (!readStackQword(snapshot, static_cast<std::uintptr_t>(next.Rsp), return_address) ||
                !windowsAddAddress(static_cast<std::uintptr_t>(next.Rsp), adjustment, next_rsp) ||
                return_address == 0 || !windowsCanonicalAddress(static_cast<std::uintptr_t>(return_address))) {
                return EpilogueResult::Ambiguous;
            }
            next.Rsp = static_cast<DWORD64>(next_rsp);
            next.Rip = return_address;
            context = next;
            return EpilogueResult::Unwound;
        }

        std::size_t length = 0;
        if ((first & 0xf8U) == 0x58U || first == 0x41U) {
            std::uint8_t opcode = first;
            if (first == 0x41U) {
                if (!read(1, &opcode, sizeof(opcode))) {
                    return EpilogueResult::Ambiguous;
                }
                if ((opcode & 0xf8U) != 0x58U) {
                    return recognized ? EpilogueResult::Ambiguous : EpilogueResult::NotEpilogue;
                }
            }
            const auto reg = static_cast<std::uint8_t>((opcode & 0x07U) + (first == 0x41U ? 8U : 0U));
            if (!validNonVolatileRegister(reg)) {
                return EpilogueResult::Ambiguous;
            }
            DWORD64 value = 0;
            std::uintptr_t next_rsp = 0;
            if (!readStackQword(snapshot, static_cast<std::uintptr_t>(next.Rsp), value) ||
                !windowsAddAddress(static_cast<std::uintptr_t>(next.Rsp), sizeof(DWORD64), next_rsp)) {
                return EpilogueResult::Ambiguous;
            }
            next.Rsp = static_cast<DWORD64>(next_rsp);
            *integerRegister(next, reg) = value;
            length = first == 0x41U ? 2U : 1U;
        }
        else if ((first & 0xf8U) == 0x48U) {
            std::uint8_t opcode = 0;
            if (!read(1, &opcode, sizeof(opcode))) {
                return EpilogueResult::Ambiguous;
            }
            if (opcode != 0x83U && opcode != 0x81U && opcode != 0x8dU) {
                return recognized ? EpilogueResult::Ambiguous : EpilogueResult::NotEpilogue;
            }
            std::uint8_t modrm = 0;
            if (!read(2, &modrm, sizeof(modrm))) {
                return EpilogueResult::Ambiguous;
            }
            if (opcode == 0x83U || opcode == 0x81U) {
                const auto destination = static_cast<std::uint8_t>((modrm & 0x07U) | ((first & 1U) != 0 ? 8U : 0U));
                if (modrm != 0xc4U || destination != 4U) {
                    return recognized ? EpilogueResult::Ambiguous : EpilogueResult::NotEpilogue;
                }
                if (recognized || first != 0x48U) {
                    return EpilogueResult::Ambiguous;
                }
                std::array<std::uint8_t, 4> immediate{};
                const std::size_t immediate_size = opcode == 0x83U ? 1U : 4U;
                if (!read(3, immediate.data(), immediate_size)) {
                    return EpilogueResult::Ambiguous;
                }
                const std::int64_t amount =
                    immediate_size == 1U ? static_cast<std::int8_t>(immediate[0])
                                         : static_cast<std::int32_t>(static_cast<std::uint32_t>(immediate[0]) |
                                                                     (static_cast<std::uint32_t>(immediate[1]) << 8U) |
                                                                     (static_cast<std::uint32_t>(immediate[2]) << 16U) |
                                                                     (static_cast<std::uint32_t>(immediate[3]) << 24U));
                std::uintptr_t adjusted = 0;
                if (amount < 0 || !windowsAddAddress(static_cast<std::uintptr_t>(next.Rsp),
                                                     static_cast<std::uint64_t>(amount), adjusted)) {
                    return EpilogueResult::Ambiguous;
                }
                next.Rsp = adjusted;
                length = 3U + immediate_size;
            }
            else {
                const auto mode = static_cast<std::uint8_t>(modrm >> 6U);
                const auto destination =
                    static_cast<std::uint8_t>(((modrm >> 3U) & 0x07U) | ((first & 4U) != 0 ? 8U : 0U));
                const auto base = static_cast<std::uint8_t>((modrm & 0x07U) | ((first & 1U) != 0 ? 8U : 0U));
                if (destination != 4U) {
                    return recognized ? EpilogueResult::Ambiguous : EpilogueResult::NotEpilogue;
                }
                if (recognized || (first != 0x48U && first != 0x49U) || (modrm & 0x07U) == 4U ||
                    !validNonVolatileRegister(base) || base != info.frame_register || (mode != 1 && mode != 2)) {
                    return EpilogueResult::Ambiguous;
                }
                std::array<std::uint8_t, 4> displacement_bytes{};
                const std::size_t displacement_size = mode == 1 ? 1U : 4U;
                if (!read(3, displacement_bytes.data(), displacement_size)) {
                    return EpilogueResult::Ambiguous;
                }
                const std::int64_t displacement =
                    displacement_size == 1U
                        ? static_cast<std::int8_t>(displacement_bytes[0])
                        : static_cast<std::int32_t>(static_cast<std::uint32_t>(displacement_bytes[0]) |
                                                    (static_cast<std::uint32_t>(displacement_bytes[1]) << 8U) |
                                                    (static_cast<std::uint32_t>(displacement_bytes[2]) << 16U) |
                                                    (static_cast<std::uint32_t>(displacement_bytes[3]) << 24U));
                const auto *base_register = integerRegister(next, base);
                std::uintptr_t adjusted = 0;
                const bool address_ok =
                    base_register != nullptr &&
                    (displacement < 0
                         ? windowsSubtractAddress(*base_register, static_cast<std::uint64_t>(-displacement), adjusted)
                         : windowsAddAddress(*base_register, static_cast<std::uint64_t>(displacement), adjusted));
                if (!address_ok) {
                    return EpilogueResult::Ambiguous;
                }
                next.Rsp = adjusted;
                length = 3U + displacement_size;
            }
        }
        else {
            return recognized ? EpilogueResult::Ambiguous : EpilogueResult::NotEpilogue;
        }
        if (length == 0 || length > scan_end - instruction) {
            return EpilogueResult::Ambiguous;
        }
        recognized = true;
        instruction += length;
    }
    return recognized ? EpilogueResult::Ambiguous : EpilogueResult::NotEpilogue;
}

}  // namespace windows_stack_snapshot_detail

[[nodiscard]] inline WindowsWalkStatus windowsUnwindNext(const WindowsStackSnapshot &snapshot, CONTEXT &context,
                                                         std::uintptr_t &instruction_pointer,
                                                         WindowsFunctionLookup lookup, void *lookup_context,
                                                         WindowsMetadataReader reader, void *reader_context) noexcept
{
    instruction_pointer = 0;
    if (lookup == nullptr || context.Rip == 0 || context.Rsp == 0 ||
        !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rip)) ||
        !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rsp))) {
        return WindowsWalkStatus::Complete;
    }

    const CONTEXT previous = context;
    WindowsRuntimeFunction function{};
    const auto lookup_status = lookup(static_cast<std::uintptr_t>(context.Rip), function, lookup_context);
    if (lookup_status == WindowsFunctionLookupStatus::Failure) {
        return WindowsWalkStatus::Complete;
    }

    if (lookup_status == WindowsFunctionLookupStatus::Leaf) {
        DWORD64 return_address = 0;
        if (!windows_stack_snapshot_detail::readStackQword(snapshot, static_cast<std::uintptr_t>(context.Rsp),
                                                           return_address) ||
            return_address == 0 || !windowsCanonicalAddress(static_cast<std::uintptr_t>(return_address))) {
            context = previous;
            return WindowsWalkStatus::Complete;
        }
        std::uintptr_t next_rsp = 0;
        if (!windowsAddAddress(static_cast<std::uintptr_t>(context.Rsp), sizeof(DWORD64), next_rsp)) {
            context = previous;
            return WindowsWalkStatus::Complete;
        }
        context.Rsp = static_cast<DWORD64>(next_rsp);
        context.Rip = return_address;
    }
    else {
        if (function.begin == 0 || function.end <= function.begin || function.unwind_info == 0 ||
            static_cast<std::uintptr_t>(context.Rip) < function.begin ||
            static_cast<std::uintptr_t>(context.Rip) >= function.end) {
            return WindowsWalkStatus::Complete;
        }
        windows_stack_snapshot_detail::ParsedUnwindInfo info{};
        if (!windows_stack_snapshot_detail::parseUnwindInfo(function, info, reader, reader_context)) {
            return WindowsWalkStatus::Complete;
        }
        const auto epilogue =
            windows_stack_snapshot_detail::unwindEpilogue(function, info, context, snapshot, reader, reader_context);
        if (epilogue == windows_stack_snapshot_detail::EpilogueResult::Ambiguous) {
            context = previous;
            return WindowsWalkStatus::Complete;
        }
        if (epilogue == windows_stack_snapshot_detail::EpilogueResult::Unwound) {
            if (context.Rip == 0 || context.Rsp == 0 ||
                !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rip)) ||
                !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rsp)) ||
                (context.Rip == previous.Rip && context.Rsp == previous.Rsp)) {
                context = previous;
                return WindowsWalkStatus::Complete;
            }
            instruction_pointer = static_cast<std::uintptr_t>(context.Rip);
            return WindowsWalkStatus::Frame;
        }

        windows_stack_snapshot_detail::ApplyState state{};
        CONTEXT next = context;
        if (!windows_stack_snapshot_detail::applyUnwindInfo(function, info, false, next, snapshot, reader,
                                                            reader_context, state)) {
            return WindowsWalkStatus::Complete;
        }
        if (!state.machine_frame) {
            if (!windows_stack_snapshot_detail::readStackQword(snapshot, static_cast<std::uintptr_t>(next.Rsp),
                                                               next.Rip)) {
                return WindowsWalkStatus::Complete;
            }
            std::uintptr_t next_rsp = 0;
            if (!windowsAddAddress(static_cast<std::uintptr_t>(next.Rsp), sizeof(DWORD64), next_rsp)) {
                return WindowsWalkStatus::Complete;
            }
            next.Rsp = static_cast<DWORD64>(next_rsp);
        }
        else if (next.Rip == 0) {
            return WindowsWalkStatus::Complete;
        }
        context = next;
    }

    if (context.Rip == 0 || context.Rsp == 0 || !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rip)) ||
        !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rsp)) ||
        (context.Rip == previous.Rip && context.Rsp == previous.Rsp)) {
        context = previous;
        return WindowsWalkStatus::Complete;
    }
    instruction_pointer = static_cast<std::uintptr_t>(context.Rip);
    return WindowsWalkStatus::Frame;
}

}  // namespace spark

#endif  // ENDSTONE_SPARK_WINDOWS_STACK_SNAPSHOT_H
