#include "native/alloc/windows_stable_entry_relocator.h"

#ifndef _WIN32
#error "windows_stable_entry_relocator.cpp must only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <distorm.h>
#include <windows.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace spark::stable_entry_experiment {
namespace {

constexpr std::size_t kDecodeWindow = 16;
constexpr std::size_t kMinPatchLength = 5;
constexpr std::size_t kMaxDecodedInstructions = 16;
constexpr std::size_t kMaxTrampolineCode = 128;
constexpr std::size_t kAllocationSize = 64 * 1024;
constexpr std::size_t kAbsoluteJumpSize = 14;

struct DecodedWindow {
    std::array<_DInst, kMaxDecodedInstructions> instructions{};
    std::size_t count = 0;
    std::size_t patch_length = 0;
};

enum class BuildStatus {
    Success,
    AddressOutOfRange,
    Unsupported,
};

bool fitsRel32(std::intptr_t difference) noexcept
{
    return difference >= static_cast<std::intptr_t>(INT32_MIN) && difference <= static_cast<std::intptr_t>(INT32_MAX);
}

bool isRetOpcode(const std::uint8_t *raw, std::size_t size) noexcept
{
    if (size == 0) {
        return false;
    }
    return raw[0] == 0xC3 || raw[0] == 0xC2 || raw[0] == 0xCB || raw[0] == 0xCA;
}

bool hasPcRelativeOperand(const _DInst &instruction) noexcept
{
    for (unsigned index = 0; index < OPERANDS_NO; ++index) {
        if (instruction.ops[index].type == O_NONE) {
            break;
        }
        if (instruction.ops[index].type == O_PC) {
            return true;
        }
    }
    return false;
}

bool decodeEntryWindow(const std::uint8_t *source, DecodedWindow &window, std::string &error)
{
    _CodeInfo info{};
    info.codeOffset = reinterpret_cast<std::uintptr_t>(source);
    info.code = source;
    info.codeLen = static_cast<int>(kDecodeWindow);
    info.dt = Decode64Bits;
    info.features = 0;

    std::array<_DInst, kMaxDecodedInstructions> decoded{};
    unsigned used = 0;
    const _DecodeResult result =
        distorm_decompose64(&info, decoded.data(), static_cast<unsigned>(decoded.size()), &used);
    if (result == DECRES_INPUTERR || result == DECRES_NONE || used == 0) {
        error = "distorm could not decode stable-entry instruction window";
        return false;
    }

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(source);
    std::size_t consumed = 0;
    for (unsigned index = 0; index < used && window.count < window.instructions.size(); ++index) {
        const _DInst &instruction = decoded[index];
        if (instruction.flags == FLAG_NOT_DECODABLE || instruction.size == 0 || instruction.addr != base + consumed ||
            consumed + instruction.size > kDecodeWindow) {
            error = "stable-entry instruction window is discontinuous or undecodable";
            return false;
        }
        window.instructions[window.count++] = instruction;
        consumed += instruction.size;
        if (consumed >= kMinPatchLength) {
            window.patch_length = consumed;
            return true;
        }
        if (isRetOpcode(source + consumed - instruction.size, instruction.size)) {
            error = "stable-entry function returns before the minimum patch window";
            return false;
        }
    }

    error = "stable-entry instruction window does not cover the minimum patch length";
    return false;
}

bool isLegacyPrefix(std::uint8_t byte) noexcept
{
    switch (byte) {
    case 0xF0:
    case 0xF2:
    case 0xF3:
    case 0x2E:
    case 0x36:
    case 0x3E:
    case 0x26:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
        return true;
    default:
        return byte >= 0x40 && byte <= 0x4F;  // REX
    }
}

bool ripDisplacementOffset(const std::uint8_t *raw, std::size_t size, std::size_t &offset) noexcept
{
    std::size_t cursor = 0;
    while (cursor < size && isLegacyPrefix(raw[cursor])) {
        ++cursor;
    }
    if (cursor >= size) {
        return false;
    }

    std::size_t modrm = 0;
    if (raw[cursor] == 0xC5) {  // two-byte VEX + opcode + ModRM
        if (cursor + 3 >= size) {
            return false;
        }
        modrm = cursor + 3;
    }
    else if (raw[cursor] == 0xC4) {  // three-byte VEX + opcode + ModRM
        if (cursor + 4 >= size) {
            return false;
        }
        modrm = cursor + 4;
    }
    else if (raw[cursor] == 0x62) {  // EVEX + opcode + ModRM
        if (cursor + 5 >= size) {
            return false;
        }
        modrm = cursor + 5;
    }
    else if (raw[cursor] == 0x0F) {
        if (cursor + 1 >= size) {
            return false;
        }
        if (raw[cursor + 1] == 0x38 || raw[cursor + 1] == 0x3A) {
            modrm = cursor + 3;
        }
        else {
            modrm = cursor + 2;
        }
    }
    else {
        modrm = cursor + 1;
    }

    if (modrm >= size || (raw[modrm] & 0xC7U) != 0x05U || modrm + 5 > size) {
        return false;
    }
    offset = modrm + 1;
    return true;
}

bool targetInsidePatch(std::uintptr_t target, std::uintptr_t source, std::size_t patch_length) noexcept
{
    return target >= source && target < source + patch_length;
}

BuildStatus encodeRel32(std::array<std::uint8_t, kMaxTrampolineCode> &output, std::size_t &out, std::uint8_t opcode,
                        std::uintptr_t target, std::uintptr_t destination_base, std::size_t instruction_size,
                        std::string &error)
{
    if (out + instruction_size > output.size()) {
        error = "stable-entry trampoline buffer capacity exceeded";
        return BuildStatus::Unsupported;
    }
    const std::uintptr_t next = destination_base + out + instruction_size;
    const std::intptr_t difference = static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(next);
    if (!fitsRel32(difference)) {
        return BuildStatus::AddressOutOfRange;
    }
    output[out] = opcode;
    const std::int32_t displacement = static_cast<std::int32_t>(difference);
    std::memcpy(output.data() + out + 1, &displacement, sizeof(displacement));
    out += instruction_size;
    return BuildStatus::Success;
}

BuildStatus buildTrampoline(const std::uint8_t *source, const DecodedWindow &window, std::uintptr_t destination_base,
                            std::array<std::uint8_t, kMaxTrampolineCode> &output, std::size_t &code_size,
                            std::string &error)
{
    const std::uintptr_t source_base = reinterpret_cast<std::uintptr_t>(source);
    std::size_t source_offset = 0;
    std::size_t out = 0;

    for (std::size_t index = 0; index < window.count; ++index) {
        const _DInst &instruction = window.instructions[index];
        const std::uint8_t *raw = source + source_offset;
        const std::size_t size = instruction.size;
        const std::uintptr_t source_next = source_base + source_offset + size;

        if (raw[0] == 0xE8 || raw[0] == 0xE9) {
            if (size != 5) {
                error = "unsupported prefixed or non-rel32 CALL/JMP encoding";
                return BuildStatus::Unsupported;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, raw + 1, sizeof(displacement));
            const std::uintptr_t target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(source_next) +
                                                                      static_cast<std::intptr_t>(displacement));
            if (targetInsidePatch(target, source_base, window.patch_length)) {
                error = "relative CALL/JMP targets the overwritten entry window";
                return BuildStatus::Unsupported;
            }
            const BuildStatus status = encodeRel32(output, out, raw[0], target, destination_base, 5, error);
            if (status != BuildStatus::Success) {
                return status;
            }
        }
        else if (raw[0] == 0xEB) {
            if (size != 2) {
                error = "unsupported short JMP encoding";
                return BuildStatus::Unsupported;
            }
            const auto displacement = static_cast<std::int8_t>(raw[1]);
            const std::uintptr_t target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(source_next) +
                                                                      static_cast<std::intptr_t>(displacement));
            if (targetInsidePatch(target, source_base, window.patch_length)) {
                error = "short JMP targets the overwritten entry window";
                return BuildStatus::Unsupported;
            }
            const BuildStatus status = encodeRel32(output, out, 0xE9, target, destination_base, 5, error);
            if (status != BuildStatus::Success) {
                return status;
            }
        }
        else if (raw[0] >= 0x70 && raw[0] <= 0x7F) {
            if (size != 2 || out + 6 > output.size()) {
                error = "unsupported short conditional branch encoding";
                return BuildStatus::Unsupported;
            }
            const auto displacement = static_cast<std::int8_t>(raw[1]);
            const std::uintptr_t target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(source_next) +
                                                                      static_cast<std::intptr_t>(displacement));
            if (targetInsidePatch(target, source_base, window.patch_length)) {
                error = "short conditional branch targets the overwritten entry window";
                return BuildStatus::Unsupported;
            }
            const std::uintptr_t next = destination_base + out + 6;
            const std::intptr_t difference = static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(next);
            if (!fitsRel32(difference)) {
                return BuildStatus::AddressOutOfRange;
            }
            output[out] = 0x0F;
            output[out + 1] = static_cast<std::uint8_t>(0x80U + (raw[0] - 0x70U));
            const std::int32_t new_displacement = static_cast<std::int32_t>(difference);
            std::memcpy(output.data() + out + 2, &new_displacement, sizeof(new_displacement));
            out += 6;
        }
        else if (raw[0] == 0x0F && size == 6 && raw[1] >= 0x80 && raw[1] <= 0x8F) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, raw + 2, sizeof(displacement));
            const std::uintptr_t target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(source_next) +
                                                                      static_cast<std::intptr_t>(displacement));
            if (targetInsidePatch(target, source_base, window.patch_length)) {
                error = "conditional rel32 branch targets the overwritten entry window";
                return BuildStatus::Unsupported;
            }
            if (out + 6 > output.size()) {
                error = "stable-entry trampoline buffer capacity exceeded";
                return BuildStatus::Unsupported;
            }
            const std::uintptr_t next = destination_base + out + 6;
            const std::intptr_t difference = static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(next);
            if (!fitsRel32(difference)) {
                return BuildStatus::AddressOutOfRange;
            }
            output[out] = 0x0F;
            output[out + 1] = raw[1];
            const std::int32_t new_displacement = static_cast<std::int32_t>(difference);
            std::memcpy(output.data() + out + 2, &new_displacement, sizeof(new_displacement));
            out += 6;
        }
        else {
            if (hasPcRelativeOperand(instruction)) {
                error = "unsupported relative control-flow encoding in stable-entry window";
                return BuildStatus::Unsupported;
            }
            if (out + size > output.size()) {
                error = "stable-entry trampoline buffer capacity exceeded";
                return BuildStatus::Unsupported;
            }
            std::memcpy(output.data() + out, raw, size);

            if ((instruction.flags & FLAG_RIP_RELATIVE) != 0) {
                if (instruction.dispSize != 32) {
                    error = "unsupported non-disp32 RIP-relative memory operand";
                    return BuildStatus::Unsupported;
                }
                std::size_t displacement_offset = 0;
                if (!ripDisplacementOffset(raw, size, displacement_offset)) {
                    error = "could not prove RIP-relative displacement byte offset";
                    return BuildStatus::Unsupported;
                }
                std::int32_t old_displacement = 0;
                std::memcpy(&old_displacement, raw + displacement_offset, sizeof(old_displacement));
                if (old_displacement != static_cast<std::int32_t>(instruction.disp)) {
                    error = "RIP-relative displacement metadata does not match executable bytes";
                    return BuildStatus::Unsupported;
                }
                const std::uintptr_t target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(source_next) +
                                                                          static_cast<std::intptr_t>(old_displacement));
                const std::uintptr_t destination_next = destination_base + out + size;
                const std::intptr_t difference =
                    static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(destination_next);
                if (!fitsRel32(difference)) {
                    return BuildStatus::AddressOutOfRange;
                }
                const std::int32_t new_displacement = static_cast<std::int32_t>(difference);
                std::memcpy(output.data() + out + displacement_offset, &new_displacement, sizeof(new_displacement));
            }
            out += size;
        }
        source_offset += size;
    }

    if (source_offset != window.patch_length || out + kAbsoluteJumpSize > output.size()) {
        error = "stable-entry relocation window accounting mismatch";
        return BuildStatus::Unsupported;
    }

    output[out + 0] = 0xFF;
    output[out + 1] = 0x25;
    output[out + 2] = 0x00;
    output[out + 3] = 0x00;
    output[out + 4] = 0x00;
    output[out + 5] = 0x00;
    const std::uint64_t return_address = static_cast<std::uint64_t>(source_base + window.patch_length);
    std::memcpy(output.data() + out + 6, &return_address, sizeof(return_address));
    out += kAbsoluteJumpSize;
    code_size = out;
    return BuildStatus::Success;
}

}  // namespace

bool prepareBoundedRelocation(void *source_address, BoundedRelocation &relocation, std::string &error)
{
    error.clear();
    relocation = {};
    if (source_address == nullptr) {
        error = "stable-entry relocation source is null";
        return false;
    }

    const auto *source = static_cast<const std::uint8_t *>(source_address);
    DecodedWindow window;
    if (!decodeEntryWindow(source, window, error)) {
        return false;
    }

    SYSTEM_INFO system{};
    ::GetSystemInfo(&system);
    const std::uintptr_t granularity =
        system.dwAllocationGranularity != 0 ? system.dwAllocationGranularity : kAllocationSize;
    const std::uintptr_t source_value = reinterpret_cast<std::uintptr_t>(source);
    const std::uintptr_t base = source_value & ~(granularity - 1);
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
            void *memory = ::VirtualAlloc(reinterpret_cast<void *>(candidate), kAllocationSize,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (memory == nullptr || reinterpret_cast<std::uintptr_t>(memory) != candidate) {
                if (memory != nullptr) {
                    ::VirtualFree(memory, 0, MEM_RELEASE);
                }
                continue;
            }

            std::array<std::uint8_t, kMaxTrampolineCode> code{};
            std::size_t code_size = 0;
            const BuildStatus status = buildTrampoline(source, window, candidate, code, code_size, error);
            if (status == BuildStatus::AddressOutOfRange) {
                ::VirtualFree(memory, 0, MEM_RELEASE);
                continue;
            }
            if (status == BuildStatus::Unsupported) {
                ::VirtualFree(memory, 0, MEM_RELEASE);
                return false;
            }

            std::memcpy(memory, code.data(), code_size);
            DWORD old_protection = 0;
            if (::VirtualProtect(memory, kAllocationSize, PAGE_EXECUTE_READ, &old_protection) == FALSE) {
                const DWORD failure = ::GetLastError();
                ::VirtualFree(memory, 0, MEM_RELEASE);
                error = "VirtualProtect bounded relocation executable failed: " + std::to_string(failure);
                return false;
            }
            if (::FlushInstructionCache(::GetCurrentProcess(), memory, code_size) == FALSE) {
                const DWORD failure = ::GetLastError();
                ::VirtualFree(memory, 0, MEM_RELEASE);
                error = "FlushInstructionCache bounded relocation failed: " + std::to_string(failure);
                return false;
            }

            relocation.memory = memory;
            relocation.entry = memory;
            relocation.allocation_size = kAllocationSize;
            relocation.code_size = code_size;
            relocation.patch_length = window.patch_length;
            return true;
        }
        if (max_distance - distance < granularity) {
            break;
        }
    }

    error = "could not reserve a relocation buffer satisfying all x64 relative ranges";
    return false;
}

void releaseBoundedRelocation(BoundedRelocation &relocation) noexcept
{
    if (relocation.memory != nullptr) {
        (void)::VirtualFree(relocation.memory, 0, MEM_RELEASE);
    }
    relocation = {};
}

}  // namespace spark::stable_entry_experiment
