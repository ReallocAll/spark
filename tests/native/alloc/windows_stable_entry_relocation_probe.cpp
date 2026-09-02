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

#include <funchook.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>

extern "C" int funchook_set_debug_file(const char *name);

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
        const auto displacement = static_cast<std::int32_t>(target - source);
        writeI32(displacement_offset, displacement);
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

void makePreparedTrampolineExecutable(void *callable)
{
    MEMORY_BASIC_INFORMATION memory{};
    assert(::VirtualQuery(callable, &memory, sizeof(memory)) != 0);
    assert(memory.BaseAddress != nullptr);
    assert(memory.RegionSize != 0);
    DWORD old_protection = 0;
    assert(::VirtualProtect(memory.BaseAddress, memory.RegionSize, PAGE_EXECUTE_READ, &old_protection) != FALSE);
    assert(::FlushInstructionCache(::GetCurrentProcess(), memory.BaseAddress, memory.RegionSize) != FALSE);
}

void assertRelocates(std::string_view name, CodePage &page, int input_a, int expected_a, int input_b, int expected_b)
{
    assert(page.function()(input_a) == expected_a);
    assert(page.function()(input_b) == expected_b);

    funchook_t *relocator = funchook_create();
    assert(relocator != nullptr);
    void *callable = page.address();
    const int code = funchook_prepare(relocator, &callable, page.address());
    if (code != FUNCHOOK_ERROR_SUCCESS) {
        const char *detail = funchook_error_message(relocator);
        std::cerr << "relocation-matrix: " << name << " unexpected prepare failure code=" << code
                  << " detail=" << (detail != nullptr ? detail : "<none>") << '\n';
        std::abort();
    }
    assert(callable != nullptr && callable != page.address());
    makePreparedTrampolineExecutable(callable);
    auto trampoline = reinterpret_cast<SyntheticFn>(callable);
    assert(trampoline(input_a) == expected_a);
    assert(trampoline(input_b) == expected_b);
    assert(funchook_destroy(relocator) == FUNCHOOK_ERROR_SUCCESS);
    std::cerr << "relocation-matrix: " << name << " supported\n";
}

void assertRejected(std::string_view name, CodePage &page)
{
    funchook_t *relocator = funchook_create();
    assert(relocator != nullptr);
    void *callable = page.address();
    const int code = funchook_prepare(relocator, &callable, page.address());
    const char *detail = funchook_error_message(relocator);
    std::cerr << "relocation-matrix: " << name << " rejected code=" << code
              << " detail=" << (detail != nullptr ? detail : "<none>") << '\n';
    assert(code != FUNCHOOK_ERROR_SUCCESS);
    assert(callable == page.address());
    assert(funchook_destroy(relocator) == FUNCHOOK_ERROR_SUCCESS);
}

void testRipRelativeMemory()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> code{
        0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,  // mov eax, dword ptr [rip+disp32]
        0x03, 0xC1,                          // add eax, ecx
        0xC3,                                // ret
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
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
    constexpr std::array<std::uint8_t, 8> entry{
        0xE8, 0x00, 0x00, 0x00, 0x00,  // call rel32
        0xC3, 0x90, 0x90,
    };
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
    constexpr std::array<std::uint8_t, 8> entry{
        0xE9, 0x00, 0x00, 0x00, 0x00,  // jmp rel32
        0xCC, 0xCC, 0xCC,
    };
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
        0x85, 0xC9,                          // test ecx, ecx
        0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,  // jz rel32
        0x8D, 0x41, 0x01,                    // lea eax, [rcx+1]
        0xC3,                                // ret
        0x90, 0x90, 0x90, 0x90,
    };
    constexpr std::array<std::uint8_t, 6> helper{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    page.write(0, entry);
    page.write(0x100, helper);
    page.writeRel32(4, 8, 0x100);
    page.finalize();
    assertRelocates("conditional-rel32", page, 0, 42, 5, 6);
}

void testShortConditionalBranchIsFailClosed()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> entry{
        0x85, 0xC9,              // test ecx, ecx
        0x74, 0x04,              // jz +4 -> mov eax,42
        0x8D, 0x41, 0x01,        // lea eax, [rcx+1]
        0xC3,                    // ret
        0xB8, 0x2A, 0x00, 0x00, 0x00,  // mov eax,42
        0xC3, 0x90, 0x90,
    };
    page.write(0, entry);
    page.finalize();
    assert(page.function()(0) == 42);
    assert(page.function()(5) == 6);
    assertRejected("conditional-rel8-needs-expansion", page);
}

void testShortJumpIsFailClosed()
{
    CodePage page;
    constexpr std::array<std::uint8_t, 16> entry{
        0xEB, 0x06,              // jmp +6 -> helper at offset 8
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x8D, 0x41, 0x0B, 0xC3,  // lea eax,[rcx+11]; ret
        0x90, 0x90, 0x90, 0x90,
    };
    page.write(0, entry);
    page.finalize();
    assert(page.function()(1) == 12);
    assertRejected("jump-rel8-needs-expansion", page);
}

}  // namespace

int main()
{
    std::cerr << "relocation-matrix: begin\n";
    assert(funchook_set_debug_file("funchook-stable-entry.log") == 0);
    testRipRelativeMemory();
    testRelativeCall();
    testRelativeJump();
    testNearConditionalBranch();
    testShortConditionalBranchIsFailClosed();
    testShortJumpIsFailClosed();
    std::cerr << "relocation-matrix: pass supported=rip-relative,call-rel32,jmp-rel32,jcc-rel32"
                 " rejected=rel8-jcc,rel8-jmp\n";
    return 0;
}
