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

std::unique_ptr<spark::WindowsAllocationIatHooks> GHooks;
MallocFn GMalloc = nullptr;
FreeFn GFree = nullptr;
std::atomic<std::uint64_t> GCalls{0};
std::atomic<bool> GHold{false};
std::atomic<bool> GEntered{false};
char GError[512]{};

void setError(const char *operation, const std::string &detail) noexcept
{
    std::snprintf(GError, sizeof(GError), "%s failed: %s", operation, detail.c_str());
}

extern "C" void *__cdecl hookMalloc(std::size_t size) noexcept
{
    GCalls.fetch_add(1, std::memory_order_relaxed);
    GEntered.store(true, std::memory_order_release);
    while (GHold.load(std::memory_order_acquire)) {
        (void)::SwitchToThread();
    }
    MallocFn original = GMalloc;
    return original != nullptr ? original(size) : nullptr;
}

extern "C" void __cdecl hookFree(void *pointer) noexcept
{
    GCalls.fetch_add(1, std::memory_order_relaxed);
    FreeFn original = GFree;
    if (original != nullptr) {
        original(pointer);
    }
}

void resetBackend() noexcept
{
    GHooks.reset();
    GMalloc = nullptr;
    GFree = nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl windowsPermanentIatBackendInstall() noexcept
{
    GError[0] = '\0';
    if (GHooks != nullptr) {
        return 1;
    }

    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    if (ucrt == nullptr) {
        std::snprintf(GError, sizeof(GError), "ucrtbase.dll is not loaded");
        return 0;
    }
    GMalloc = reinterpret_cast<MallocFn>(::GetProcAddress(ucrt, "malloc"));
    GFree = reinterpret_cast<FreeFn>(::GetProcAddress(ucrt, "free"));
    if (GMalloc == nullptr || GFree == nullptr) {
        std::snprintf(GError, sizeof(GError), "required UCRT allocator exports are unavailable");
        resetBackend();
        return 0;
    }

    try {
        GHooks = std::make_unique<spark::WindowsAllocationIatHooks>();
        std::string error;
        if (!GHooks->addTarget(reinterpret_cast<void *>(GMalloc), reinterpret_cast<void *>(&hookMalloc), error)) {
            setError("addTarget(malloc)", error);
            resetBackend();
            return 0;
        }
        if (!GHooks->addTarget(reinterpret_cast<void *>(GFree), reinterpret_cast<void *>(&hookFree), error)) {
            setError("addTarget(free)", error);
            resetBackend();
            return 0;
        }
        if (!GHooks->install(error)) {
            setError("install", error);
            resetBackend();
            return 0;
        }
        return 1;
    }
    catch (...) {
        std::snprintf(GError, sizeof(GError), "allocation IAT backend setup threw an exception");
        resetBackend();
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl windowsPermanentIatBackendUninstall() noexcept
{
    GError[0] = '\0';
    if (GHooks == nullptr) {
        return 1;
    }
    std::string error;
    if (!GHooks->uninstall(error)) {
        setError("uninstall", error);
        return 0;
    }
    resetBackend();
    return 1;
}

extern "C" __declspec(dllexport) const char *__cdecl windowsPermanentIatBackendError() noexcept
{
    return GError;
}

extern "C" __declspec(dllexport) std::uint64_t __cdecl windowsPermanentIatBackendCalls() noexcept
{
    return GCalls.load(std::memory_order_acquire);
}

extern "C" __declspec(dllexport) void __cdecl windowsPermanentIatBackendSetHold(int enabled) noexcept
{
    GHold.store(enabled != 0, std::memory_order_release);
}

extern "C" __declspec(dllexport) void __cdecl windowsPermanentIatBackendResetEntered() noexcept
{
    GEntered.store(false, std::memory_order_release);
}

extern "C" __declspec(dllexport) int __cdecl windowsPermanentIatBackendEntered() noexcept
{
    return GEntered.load(std::memory_order_acquire) ? 1 : 0;
}
