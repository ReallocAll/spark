#ifndef _WIN32
#error "windows_gateway_test_handler.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>

namespace {

constexpr int KSpecialValue = 0x60000000;

std::atomic<bool> GHold{false};
std::atomic<bool> GEntered{false};
std::atomic<bool> GSpecialEntered{false};

}  // namespace

extern "C" __declspec(dllexport) int __cdecl windowsGatewayTestHandler(int value) noexcept
{
    GEntered.store(true, std::memory_order_release);
    if (value == KSpecialValue) {
        GSpecialEntered.store(true, std::memory_order_release);
    }
    while (GHold.load(std::memory_order_acquire)) {
        (void)::SwitchToThread();
    }
    return value + 1000;
}

extern "C" __declspec(dllexport) void __cdecl windowsGatewayTestSetHold(int enabled) noexcept
{
    GHold.store(enabled != 0, std::memory_order_release);
}

extern "C" __declspec(dllexport) void __cdecl windowsGatewayTestResetEntered() noexcept
{
    GEntered.store(false, std::memory_order_release);
}

extern "C" __declspec(dllexport) int __cdecl windowsGatewayTestEntered() noexcept
{
    return GEntered.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" __declspec(dllexport) void __cdecl windowsGatewayTestResetSpecialEntered() noexcept
{
    GSpecialEntered.store(false, std::memory_order_release);
}

extern "C" __declspec(dllexport) int __cdecl windowsGatewayTestSpecialEntered() noexcept
{
    return GSpecialEntered.load(std::memory_order_acquire) ? 1 : 0;
}
