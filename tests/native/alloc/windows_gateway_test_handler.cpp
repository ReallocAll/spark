#ifndef _WIN32
#error "windows_gateway_test_handler.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>

namespace {

constexpr int kSpecialValue = 0x60000000;

std::atomic<bool> g_hold{false};
std::atomic<bool> g_entered{false};
std::atomic<bool> g_special_entered{false};

}  // namespace

extern "C" __declspec(dllexport) int __cdecl windowsGatewayTestHandler(int value) noexcept
{
    g_entered.store(true, std::memory_order_release);
    if (value == kSpecialValue) {
        g_special_entered.store(true, std::memory_order_release);
    }
    while (g_hold.load(std::memory_order_acquire)) {
        (void)::SwitchToThread();
    }
    return value + 1000;
}

extern "C" __declspec(dllexport) void __cdecl windowsGatewayTestSetHold(int enabled) noexcept
{
    g_hold.store(enabled != 0, std::memory_order_release);
}

extern "C" __declspec(dllexport) void __cdecl windowsGatewayTestResetEntered() noexcept
{
    g_entered.store(false, std::memory_order_release);
}

extern "C" __declspec(dllexport) int __cdecl windowsGatewayTestEntered() noexcept
{
    return g_entered.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" __declspec(dllexport) void __cdecl windowsGatewayTestResetSpecialEntered() noexcept
{
    g_special_entered.store(false, std::memory_order_release);
}

extern "C" __declspec(dllexport) int __cdecl windowsGatewayTestSpecialEntered() noexcept
{
    return g_special_entered.load(std::memory_order_acquire) ? 1 : 0;
}
