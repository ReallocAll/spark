#ifndef _WIN32
#error "windows_permanent_gateway_test_handler.cpp is Windows-only"
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace {
using SyntheticFn = int(__cdecl *)(int);

SyntheticFn g_original = nullptr;
volatile LONG64 *g_calls = nullptr;
volatile LONG64 *g_entered = nullptr;
volatile LONG64 *g_stack_ok = nullptr;
volatile LONG *g_hold = nullptr;
volatile LONG g_stack_checked = 0;
std::uintptr_t g_gateway_begin = 0;
std::uintptr_t g_gateway_end = 0;

[[nodiscard]] bool inGateway(const void *address) noexcept
{
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);
    return g_gateway_begin != 0 && g_gateway_begin <= value && value < g_gateway_end;
}
}  // namespace

extern "C" __declspec(dllexport) BOOL __cdecl configure_gateway_handler(
    void *original, volatile LONG64 *calls, volatile LONG64 *entered, volatile LONG64 *stack_ok, volatile LONG *hold,
    void *gateway_begin, std::size_t gateway_size) noexcept
{
    if (original == nullptr || calls == nullptr || entered == nullptr || stack_ok == nullptr || hold == nullptr ||
        gateway_begin == nullptr || gateway_size == 0) {
        return FALSE;
    }
    g_original = reinterpret_cast<SyntheticFn>(original);
    g_calls = calls;
    g_entered = entered;
    g_stack_ok = stack_ok;
    g_hold = hold;
    g_gateway_begin = reinterpret_cast<std::uintptr_t>(gateway_begin);
    g_gateway_end = g_gateway_begin + gateway_size;
    (void)::InterlockedExchange(&g_stack_checked, 0);
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl gateway_test_handler(int value) noexcept
{
    ::InterlockedIncrement64(g_entered);

    if (::InterlockedCompareExchange(&g_stack_checked, 1, 0) == 0) {
        void *frames[16]{};
        const USHORT frame_count = ::RtlCaptureStackBackTrace(0, 16, frames, nullptr);
        bool saw_gateway = false;
        for (USHORT index = 0; index < frame_count; ++index) {
            if (inGateway(frames[index])) {
                saw_gateway = true;
                break;
            }
        }
        if (saw_gateway) {
            ::InterlockedIncrement64(g_stack_ok);
        }
    }

    while (::InterlockedCompareExchange(g_hold, 0, 0) != 0) {
        ::SwitchToThread();
    }

    const int result = g_original(value);
    ::InterlockedIncrement64(g_calls);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
