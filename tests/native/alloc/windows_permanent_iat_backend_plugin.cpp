#include <funchook.h>

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

namespace {

using MallocFn = void *(__cdecl *)(std::size_t);
using FreeFn = void(__cdecl *)(void *);

funchook_t *g_hooks = nullptr;
MallocFn g_malloc = nullptr;
FreeFn g_free = nullptr;
std::atomic<std::uint64_t> g_calls{0};
std::atomic<bool> g_hold{false};
std::atomic<bool> g_entered{false};
char g_error[512]{};

void setError(const char *operation, int code) noexcept
{
    const char *detail = g_hooks != nullptr ? funchook_error_message(g_hooks) : nullptr;
    std::snprintf(g_error, sizeof(g_error), "%s failed code=%d detail=%s", operation, code,
                  detail != nullptr ? detail : "");
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
        g_malloc = nullptr;
        g_free = nullptr;
        return 0;
    }

    g_hooks = funchook_create();
    if (g_hooks == nullptr) {
        std::snprintf(g_error, sizeof(g_error), "funchook_create failed");
        g_malloc = nullptr;
        g_free = nullptr;
        return 0;
    }

    int code = funchook_prepare(g_hooks, reinterpret_cast<void **>(&g_malloc), reinterpret_cast<void *>(&hookMalloc));
    if (code != FUNCHOOK_ERROR_SUCCESS) {
        setError("funchook_prepare(malloc)", code);
        (void)funchook_destroy(g_hooks);
        g_hooks = nullptr;
        g_malloc = nullptr;
        g_free = nullptr;
        return 0;
    }
    code = funchook_prepare(g_hooks, reinterpret_cast<void **>(&g_free), reinterpret_cast<void *>(&hookFree));
    if (code != FUNCHOOK_ERROR_SUCCESS) {
        setError("funchook_prepare(free)", code);
        (void)funchook_destroy(g_hooks);
        g_hooks = nullptr;
        g_malloc = nullptr;
        g_free = nullptr;
        return 0;
    }
    code = funchook_install(g_hooks, 0);
    if (code != FUNCHOOK_ERROR_SUCCESS) {
        setError("funchook_install", code);
        (void)funchook_destroy(g_hooks);
        g_hooks = nullptr;
        g_malloc = nullptr;
        g_free = nullptr;
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl windowsPermanentIatBackendUninstall() noexcept
{
    g_error[0] = '\0';
    if (g_hooks == nullptr) {
        return 1;
    }
    const int uninstall_code = funchook_uninstall(g_hooks, 0);
    if (uninstall_code != FUNCHOOK_ERROR_SUCCESS) {
        setError("funchook_uninstall", uninstall_code);
        return 0;
    }
    const int destroy_code = funchook_destroy(g_hooks);
    if (destroy_code != FUNCHOOK_ERROR_SUCCESS) {
        setError("funchook_destroy", destroy_code);
        return 0;
    }
    g_hooks = nullptr;
    g_malloc = nullptr;
    g_free = nullptr;
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
