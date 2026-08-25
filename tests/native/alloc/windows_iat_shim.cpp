#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native/alloc/windows_callback_gate.h"

namespace {

using AllocFn = void *(__cdecl *)(std::size_t);

spark::WindowsCallbackLifetimeGate gGate;
std::atomic<AllocFn> gOriginal{nullptr};
std::atomic<AllocFn> gHandler{nullptr};
std::atomic<std::uint64_t> gHandlerCalls{0};
std::atomic<std::uint64_t> gFallbackCalls{0};

}  // namespace

extern "C" __declspec(dllexport) int sparkIatShimPin() noexcept
{
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<LPCWSTR>(reinterpret_cast<std::uintptr_t>(&sparkIatShimPin));
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    return ::GetModuleHandleExW(flags, address, &module) != FALSE ? 1 : 0;
}

extern "C" __declspec(dllexport) int sparkIatShimConfigure(AllocFn original) noexcept
{
    if (original == nullptr || !gGate.drained()) {
        return 0;
    }
    gOriginal.store(original, std::memory_order_release);
    return 1;
}

extern "C" __declspec(dllexport) int sparkIatShimActivate(AllocFn handler) noexcept
{
    if (handler == nullptr || !gGate.drained()) {
        return 0;
    }
    gHandler.store(handler, std::memory_order_release);
    if (!gGate.open()) {
        gHandler.store(nullptr, std::memory_order_release);
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) int sparkIatShimBeginDeactivate() noexcept
{
    return gGate.close() ? 1 : 0;
}

extern "C" __declspec(dllexport) int sparkIatShimDrained() noexcept
{
    return gGate.drained() ? 1 : 0;
}

extern "C" __declspec(dllexport) int sparkIatShimFinishDeactivate() noexcept
{
    if (!gGate.drained()) {
        return 0;
    }
    gHandler.store(nullptr, std::memory_order_release);
    return 1;
}

extern "C" __declspec(dllexport) void *__cdecl sparkIatShimAlloc(std::size_t size) noexcept
{
    AllocFn original = gOriginal.load(std::memory_order_acquire);
    if (original == nullptr) {
        return nullptr;
    }

    if (!gGate.tryEnter()) {
        gFallbackCalls.fetch_add(1, std::memory_order_relaxed);
        return original(size);
    }

    AllocFn handler = gHandler.load(std::memory_order_acquire);
    void *result = nullptr;
    if (handler == nullptr) {
        gFallbackCalls.fetch_add(1, std::memory_order_relaxed);
        result = original(size);
    }
    else {
        gHandlerCalls.fetch_add(1, std::memory_order_relaxed);
        result = handler(size);
    }
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) std::uint64_t sparkIatShimHandlerCalls() noexcept
{
    return gHandlerCalls.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t sparkIatShimFallbackCalls() noexcept
{
    return gFallbackCalls.load(std::memory_order_relaxed);
}
