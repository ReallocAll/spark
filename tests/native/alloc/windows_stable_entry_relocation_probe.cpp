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
#include <cstdint>
#include <cstring>
#include <iostream>

extern "C" int funchook_set_debug_file(const char *name);

namespace {

class SyntheticCode {
public:
    SyntheticCode()
    {
        page_ = static_cast<std::uint8_t *>(
            ::VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        assert(page_ != nullptr);
        constexpr std::array<std::uint8_t, 16> code{
            0x8D, 0x41, 0x01, 0xC3, 0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        };
        std::memcpy(page_, code.data(), code.size());
        assert(::FlushInstructionCache(::GetCurrentProcess(), page_, code.size()) != FALSE);
        DWORD old_protection = 0;
        assert(::VirtualProtect(page_, 64 * 1024, PAGE_EXECUTE_READ, &old_protection) != FALSE);
    }

    ~SyntheticCode()
    {
        if (page_ != nullptr) {
            ::VirtualFree(page_, 0, MEM_RELEASE);
        }
    }

    void *address() const noexcept { return page_; }

private:
    std::uint8_t *page_ = nullptr;
};

}  // namespace

int main()
{
    std::cerr << "relocation-probe: begin\n";
    assert(funchook_set_debug_file("funchook-stable-entry.log") == 0);
    SyntheticCode target;
    funchook_t *relocator = funchook_create();
    assert(relocator != nullptr);
    std::cerr << "relocation-probe: created target=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(target.address()) << std::dec << '\n';

    void *callable = target.address();
    std::cerr << "relocation-probe: prepare-enter\n";
    const int code = funchook_prepare(relocator, &callable, target.address());
    std::cerr << "relocation-probe: prepare-return code=" << code << " callable=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(callable) << std::dec << '\n';
    if (code != FUNCHOOK_ERROR_SUCCESS) {
        const char *detail = funchook_error_message(relocator);
        std::cerr << "relocation-probe: failure=" << (detail != nullptr ? detail : "<none>") << '\n';
        return 2;
    }
    assert(callable != nullptr);
    assert(callable != target.address());
    const int destroy_code = funchook_destroy(relocator);
    std::cerr << "relocation-probe: destroy=" << destroy_code << '\n';
    assert(destroy_code == FUNCHOOK_ERROR_SUCCESS);
    std::cerr << "relocation-probe: pass\n";
    return 0;
}
