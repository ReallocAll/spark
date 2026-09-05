#include "native/alloc/windows_stable_entry_relocator.h"

#ifndef _WIN32
#error "windows_stable_entry_relocation_probe.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

using spark::stable_entry_experiment::BoundedRelocation;
using spark::stable_entry_experiment::prepareBoundedRelocation;
using spark::stable_entry_experiment::releaseBoundedRelocation;

namespace {

using SyntheticFn = int(__cdecl *)(int);

class CodePage {
public:
    CodePage()
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(page_ != nullptr);
        assert((reinterpret_cast<std::uintptr_t>(page_) & 15U) == 0);
        std::memset(page_, 0xCC, 64 * 1024);
    }

    ~CodePage()
    {
        if (page_ != nullptr) {
            ::VirtualFree(page_, 0, MEM_RELEASE);
        }
    }

    template <std::size_t N>
    void write(std::size_t offset, const std::array<std::uint8_t, N> &bytes)
    {
        assert(offset + bytes.size() <= 64 * 1024);
        std::memcpy(page_ + offset, bytes.data(), bytes.size());
    }

    void writeI32(std::size_t offset, std::int32_t value)
    {
        assert(offset + sizeof(value) <= 64 * 1024);
        std::memcpy(page_ + offset, &value, sizeof(value));
    }

    void writeRel32(std::size_t displacement_offset, std::size_t instruction_end_offset, std::size_t target_offset)
    {
        const std::intptr_t source = reinterpret_cast<std::intptr_t>(page_ + instruction_end_offset);
        const std::intptr_t target = reinterpret_cast<std::intptr_t>(page_ + target_offset);
        writeI32(displacement_offset, static_cast<std::int32_t>(target - source));
    }

    void finalize()
    {
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, 64 * 1024) != FALSE);
        DWORD old_protection = 0;
        assert(::VirtualProtect(page_, 64 * 1024, PAGE_EXECUTE_READ, &old_protection) != FALSE);
    }

    [[nodiscard]] void *address() const noexcept { return page_; }
    [[nodiscard]] SyntheticFn function() const noexcept { return reinterpret_cast<SyntheticFn>(page_); }

private:
    std::uint8_t *page_ = nullptr;
};

void assertRelocates(std::string_view name, CodePage &page, int input_a, int expected_a, int input_b, int expected_b)
{
    assert(page.function()(input_a) == expected_a);
    assert(page.function()(input_b) == expected_b);

    BoundedRelocation relocation;
    std::string error;
    if (!prepareBoundedRelocation(page.address(), relocation, error)) {
        std::cerr << "relocation-matrix: " << name << " unexpected prepare failure: " << error << '\n';
        std::abort();
    }
    assert(relocation.entry != nullptr);
    assert(relocation.memory != nullptr);
    assert(relocation.patch_length >= 5 && relocation.patch_length <= 16);
    assert(relocation.code_size > relocation.patch_length);
    auto trampoline = reinterpret_cast<SyntheticFn>(relocation.entry);
    assert(trampoline(input_a) == expected_a);
    assert(trampoline(input_b) == expected_b);
    std::cerr << "relocation-matrix: " << name << " supported patch_length=" << relocation.patch_length
              << " code_size=" << relocation.code_size << '\n';
    releaseBoundedRelocation(relocation);
    assert(relocation.memory == nullptr);
}

void assertRejected(std::string_view name, CodePage &page)
{
    BoundedRelocation relocation;
    std::string error;
    const bool prepared = prepareBoundedRelocation(page.address(), relocation, error);
    std::cerr << "relocation-matrix: " << name << " rejected=" << (!prepared) << " detail=" << error << '\n';
    assert(!prepared);
    assert(relocation.memory == nullptr);
    assert(!error.empty());
}

void testRipRelativeMemory()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> code{
        0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x03, 0xC1, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    };
    page.write(0, code);
    page.writeI32(0x100, 10);
    page.writeRel32(2, 6, 0x100);
    page.finalize();
    assertRelocates("rip-relative-memory", page, 5, 15, -3, 7);
}

void testRelativeCall()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 8> entry{0xE8, 0, 0, 0, 0, 0xC3, 0x90, 0x90};
    constexpr std::array<std::uint8_t, 4> helper{0x8D, 0x41, 0x07, 0xC3};
    page.write(0, entry);
    page.write(0x100, helper);
    page.writeRel32(1, 5, 0x100);
    page.finalize();
    assertRelocates("relative-call", page, 3, 10, 20, 27);
}

void testRelativeJump()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 8> entry{0xE9, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC};
    constexpr std::array<std::uint8_t, 4> helper{0x8D, 0x41, 0x09, 0xC3};
    page.write(0, entry);
    page.write(0x100, helper);
    page.writeRel32(1, 5, 0x100);
    page.finalize();
    assertRelocates("relative-jump", page, 1, 10, 11, 20);
}

void testNearConditionalBranch()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> entry{
        0x85, 0xC9, 0x0F, 0x84, 0, 0, 0, 0, 0x8D, 0x41, 0x01, 0xC3, 0x90, 0x90, 0x90, 0x90,
    };
    constexpr std::array<std::uint8_t, 6> helper{0xB8, 0x2A, 0, 0, 0, 0xC3};
    page.write(0, entry);
    page.write(0x100, helper);
    page.writeRel32(4, 8, 0x100);
    page.finalize();
    assertRelocates("conditional-rel32", page, 0, 42, 5, 6);
}

void testShortConditionalBranchExpansion()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> entry{
        0x85, 0xC9, 0x74, 0x04, 0x8D, 0x41, 0x01, 0xC3, 0xB8, 0x2A, 0, 0, 0, 0xC3, 0x90, 0x90,
    };
    page.write(0, entry);
    page.finalize();
    assertRelocates("conditional-rel8-expanded", page, 0, 42, 5, 6);
}

void testShortJumpExpansion()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> entry{
        0xEB, 0x06, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x8D, 0x41, 0x0B, 0xC3, 0x90, 0x90, 0x90, 0x90,
    };
    page.write(0, entry);
    page.finalize();
    assertRelocates("jump-rel8-expanded", page, 1, 12, 10, 21);
}

void testLoopFamilyFailClosed()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> entry{
        0xE2, 0x06,  // loop +6: rel8 LOOP semantics are intentionally not rewritten yet.
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x8D, 0x41, 0x0D, 0xC3, 0x90, 0x90, 0x90, 0x90,
    };
    page.write(0, entry);
    page.finalize();
    assertRejected("loop-rel8-unsupported", page);
}

void testBranchIntoPatchWindowFailClosed()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> entry{
        0xEB, 0x00,  // jmp to offset 2, which lies in the bytes Spark would replace.
        0x90, 0x90, 0x90, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    };
    page.write(0, entry);
    page.finalize();
    assertRejected("branch-into-patch-window", page);
}

}  // namespace

int main()
{
    std::cerr << "relocation-matrix: begin\n";
    testRipRelativeMemory();
    testRelativeCall();
    testRelativeJump();
    testNearConditionalBranch();
    testShortConditionalBranchExpansion();
    testShortJumpExpansion();
    testLoopFamilyFailClosed();
    testBranchIntoPatchWindowFailClosed();
    std::cerr << "relocation-matrix: pass supported=rip-relative,call-rel32,jmp-rel32,jcc-rel32,jcc-rel8,jmp-rel8"
                 " rejected=loop-family,branch-into-patch\n";
    return 0;
}
