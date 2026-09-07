#include "native/alloc/windows_allocation_iat_hooks.h"

#ifndef _WIN32
#error "windows_permanent_iat_backend_plugin.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {

using MallocFn = void *(__cdecl *)(std::size_t);
using FreeFn = void(__cdecl *)(void *);

std::unique_ptr<spark::WindowsAllocationIatHooks> g_hooks;
MallocFn g_malloc = nullptr;
FreeFn g_free = nullptr;
std::atomic<std::uint64_t> g_calls{0};
std::atomic<bool> g_hold{false};
std::atomic<bool> g_entered{false};
char g_error[512]{};

void setError(const char *operation, const std::string &detail) noexcept
{
    std::snprintf(g_error, sizeof(g_error), "%s failed: %s", operation, detail.c_str());
}

extern "C" void *__cdecl hookMalloc(std::size_t size) noexcept
{
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_entered.store(true, std::memory_order_release);
    while (g_hold.load(std::memory_order_acquire)) {
        (void)::SwitchToThread();
    }
    MallocFn original = g_malloc;
    return original != nullptr ? original(size) : nullptr;
}

extern "C" void __cdecl hookFree(void *pointer) noexcept
{
    g_calls.fetch_add(1, std::memory_order_relaxed);
    FreeFn original = g_free;
    if (original != nullptr) {
        original(pointer);
    }
}

void resetBackend() noexcept
{
    g_hooks.reset();
    g_malloc = nullptr;
    g_free = nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl windowsPermanentIatBackendInstall() noexcept
{
    g_error[0] = '\0';
    if (g_hooks != nullptr) {
        return 1;
    }

    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    if (ucrt == nullptr) {
        std::snprintf(g_error, sizeof(g_error), "ucrtbase.dll is not loaded");
        return 0;
    }
    g_malloc = reinterpret_cast<MallocFn>(::GetProcAddress(ucrt, "malloc"));
    g_free = reinterpret_cast<FreeFn>(::GetProcAddress(ucrt, "free"));
    if (g_malloc == nullptr || g_free == nullptr) {
        std::snprintf(g_error, sizeof(g_error), "required UCRT allocator exports are unavailable");
        resetBackend();
        return 0;
    }

    try {
        g_hooks = std::make_unique<spark::WindowsAllocationIatHooks>();
        std::string error;
        if (!g_hooks->addTarget(reinterpret_cast<void *>(g_malloc), reinterpret_cast<void *>(&hookMalloc), error)) {
            setError("addTarget(malloc)", error);
            resetBackend();
            return 0;
        }
        if (!g_hooks->addTarget(reinterpret_cast<void *>(g_free), reinterpret_cast<void *>(&hookFree), error)) {
            setError("addTarget(free)", error);
            resetBackend();
            return 0;
        }
        if (!g_hooks->install(error)) {
            setError("install", error);
            resetBackend();
            return 0;
        }
        return 1;
    }
    catch (...) {
        std::snprintf(g_error, sizeof(g_error), "allocation IAT backend setup threw an exception");
        resetBackend();
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl windowsPermanentIatBackendUninstall() noexcept
{
    g_error[0] = '\0';
    if (g_hooks == nullptr) {
        return 1;
    }
    std::string error;
    if (!g_hooks->uninstall(error)) {
        setError("uninstall", error);
        return 0;
    }
    resetBackend();
    return 1;
}

extern "C" __declspec(dllexport) const char *__cdecl windowsPermanentIatBackendError() noexcept
{
    return g_error;
}

extern "C" __declspec(dllexport) std::uint64_t __cdecl windowsPermanentIatBackendCalls() noexcept
{
    return g_calls.load(std::memory_order_acquire);
}

extern "C" __declspec(dllexport) void __cdecl windowsPermanentIatBackendSetHold(int enabled) noexcept
{
    g_hold.store(enabled != 0, std::memory_order_release);
}

extern "C" __declspec(dllexport) void __cdecl windowsPermanentIatBackendResetEntered() noexcept
{
    g_entered.store(false, std::memory_order_release);
}

extern "C" __declspec(dllexport) int __cdecl windowsPermanentIatBackendEntered() noexcept
{
    return g_entered.load(std::memory_order_acquire) ? 1 : 0;
}
