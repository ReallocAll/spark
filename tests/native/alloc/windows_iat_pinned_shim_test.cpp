#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native/alloc/windows_iat_hooks.h"

namespace {

using AllocFn = void *(__cdecl *)(std::size_t);
using ConsumerAllocFn = void *(__cdecl *)(std::size_t);
using ConsumerFreeFn = void(__cdecl *)(void *);
using IntFn = int(__cdecl *)();
using ConfigureFn = int(__cdecl *)(AllocFn);
using ActivateFn = int(__cdecl *)(AllocFn);
using CounterFn = std::uint64_t(__cdecl *)();
using SetBlockEventsFn = void(__cdecl *)(HANDLE, HANDLE);

bool require(bool condition, const char *message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "pinned Windows IAT shim: %s\n", message);
    return false;
}

HMODULE load(const wchar_t *name)
{
    HMODULE module = ::LoadLibraryW(name);
    if (module == nullptr) {
        std::fprintf(stderr, "pinned Windows IAT shim: LoadLibraryW failed for %ls: %lu\n", name,
                     static_cast<unsigned long>(::GetLastError()));
    }
    return module;
}

template <typename Function>
Function function(HMODULE module, const char *name)
{
    return reinterpret_cast<Function>(::GetProcAddress(module, name));
}

bool allocateAndFree(ConsumerAllocFn allocate, ConsumerFreeFn release)
{
    void *memory = allocate(96);
    if (!require(memory != nullptr, "consumer allocation returned null")) {
        return false;
    }
    release(memory);
    return true;
}

}  // namespace

int main()
{
    HMODULE provider = load(L".\\windows_iat_provider.dll");
    HMODULE consumer = load(L".\\windows_iat_consumer.dll");
    HMODULE shim = load(L".\\windows_iat_shim.dll");
    HMODULE handler = load(L".\\windows_iat_handler.dll");
    if (provider == nullptr || consumer == nullptr || shim == nullptr || handler == nullptr) {
        return 1;
    }

    AllocFn original = function<AllocFn>(provider, "sparkIatProviderAlloc");
    ConsumerAllocFn consumer_alloc = function<ConsumerAllocFn>(consumer, "sparkIatConsumerAlloc");
    ConsumerFreeFn consumer_free = function<ConsumerFreeFn>(consumer, "sparkIatConsumerFree");
    AllocFn shim_alloc = function<AllocFn>(shim, "sparkIatShimAlloc");
    IntFn shim_pin = function<IntFn>(shim, "sparkIatShimPin");
    ConfigureFn shim_configure = function<ConfigureFn>(shim, "sparkIatShimConfigure");
    ActivateFn shim_activate = function<ActivateFn>(shim, "sparkIatShimActivate");
    IntFn shim_begin_deactivate = function<IntFn>(shim, "sparkIatShimBeginDeactivate");
    IntFn shim_drained = function<IntFn>(shim, "sparkIatShimDrained");
    IntFn shim_finish_deactivate = function<IntFn>(shim, "sparkIatShimFinishDeactivate");
    CounterFn shim_handler_calls = function<CounterFn>(shim, "sparkIatShimHandlerCalls");
    CounterFn shim_fallback_calls = function<CounterFn>(shim, "sparkIatShimFallbackCalls");
    AllocFn handler_alloc = function<AllocFn>(handler, "sparkIatHandlerAlloc");
    CounterFn handler_calls = function<CounterFn>(handler, "sparkIatHandlerCalls");
    SetBlockEventsFn handler_set_block = function<SetBlockEventsFn>(handler, "sparkIatHandlerSetBlockEvents");

    if (!require(original != nullptr && consumer_alloc != nullptr && consumer_free != nullptr,
                 "provider or consumer exports are unavailable") ||
        !require(shim_alloc != nullptr && shim_pin != nullptr && shim_configure != nullptr &&
                     shim_activate != nullptr && shim_begin_deactivate != nullptr && shim_drained != nullptr &&
                     shim_finish_deactivate != nullptr && shim_handler_calls != nullptr &&
                     shim_fallback_calls != nullptr,
                 "shim exports are unavailable") ||
        !require(handler_alloc != nullptr && handler_calls != nullptr && handler_set_block != nullptr,
                 "handler exports are unavailable") ||
        !require(shim_pin() == 1, "shim could not pin itself for process lifetime") ||
        !require(shim_configure(original) == 1, "shim original target configuration failed") ||
        !require(shim_activate(handler_alloc) == 1, "shim handler activation failed")) {
        return 1;
    }

    spark::WindowsIatHooks hooks(spark::makeNativeWindowsIatHookBackend(reinterpret_cast<void *>(handler_alloc)));
    spark::WindowsIatHookTarget target;
    target.import_name = "sparkIatProviderAlloc";
    target.import_modules = {"windows_iat_provider.dll"};
    target.original = reinterpret_cast<void *>(original);
    target.replacement = reinterpret_cast<void *>(shim_alloc);
    target.required = true;

    std::string error;
    if (!require(hooks.configure({target}, error), "IAT hook configuration failed") ||
        !require(hooks.install(error), error.empty() ? "IAT install failed" : error.c_str()) ||
        !require(hooks.activeSlotCount() == 1, "test should own only the consumer import slot")) {
        return 1;
    }

    const std::uint64_t initial_handler_calls = handler_calls();
    const std::uint64_t initial_shim_handler_calls = shim_handler_calls();
    if (!allocateAndFree(consumer_alloc, consumer_free) ||
        !require(handler_calls() == initial_handler_calls + 1, "active handler was not called") ||
        !require(shim_handler_calls() == initial_shim_handler_calls + 1, "shim did not dispatch to handler")) {
        return 1;
    }

    HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!require(entered != nullptr && release != nullptr, "failed to create deterministic block events")) {
        return 1;
    }
    handler_set_block(entered, release);

    bool worker_ok = false;
    std::thread worker([&] { worker_ok = allocateAndFree(consumer_alloc, consumer_free); });
    if (!require(::WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0, "handler did not enter") ||
        !require(shim_begin_deactivate() == 1, "failed to close shim callback admission") ||
        !require(shim_drained() == 0, "shim reported drained while a callback was in flight")) {
        (void)::SetEvent(release);
        worker.join();
        return 1;
    }

    (void)::SetEvent(release);
    worker.join();
    if (!require(worker_ok, "in-flight allocation failed") ||
        !require(shim_drained() == 1, "shim did not drain after in-flight callback returned") ||
        !require(shim_finish_deactivate() == 1, "shim could not clear the drained handler")) {
        return 1;
    }
    handler_set_block(nullptr, nullptr);

    const std::uint64_t handler_calls_before_unload = handler_calls();
    const std::uint64_t fallback_before_unload = shim_fallback_calls();
    if (!require(::FreeLibrary(handler) != FALSE, "handler FreeLibrary failed")) {
        return 1;
    }
    handler = nullptr;
    if (!require(::GetModuleHandleW(L"windows_iat_handler.dll") == nullptr, "handler DLL remained loaded") ||
        !require(::FreeLibrary(shim) != FALSE, "dropping the ordinary shim reference failed") ||
        !require(::GetModuleHandleW(L"windows_iat_shim.dll") != nullptr, "pinned shim unexpectedly unloaded")) {
        return 1;
    }
    shim = nullptr;

    // The consumer IAT still points at the shim here. The plugin-like handler is
    // already physically unloaded, so this call proves the replacement cannot
    // jump into plugin-owned code after close + drain.
    if (!allocateAndFree(consumer_alloc, consumer_free) ||
        !require(shim_handler_calls() >= initial_shim_handler_calls + 2, "in-flight dispatch was not accounted") ||
        !require(shim_fallback_calls() == fallback_before_unload + 1,
                 "closed shim did not fall back to the original allocator")) {
        return 1;
    }

    if (!require(hooks.uninstall(error), error.empty() ? "IAT uninstall failed" : error.c_str()) ||
        !require(!hooks.installed() && !hooks.unsafeState(), "IAT detach did not prove clean ownership release")) {
        return 1;
    }

    // Simulate a thread that prefetched the replacement before IAT restore and
    // invokes it later. The pinned shim remains executable and must still use the
    // process-lifetime original allocator even though the handler DLL is gone.
    void *stale_memory = shim_alloc(128);
    if (!require(stale_memory != nullptr, "stale replacement pointer could not fall back safely") ||
        !require(shim_fallback_calls() == fallback_before_unload + 2,
                 "stale replacement did not use the fallback path")) {
        return 1;
    }
    consumer_free(stale_memory);

    if (!require(allocateAndFree(consumer_alloc, consumer_free), "consumer failed after clean IAT detach") ||
        !require(handler_calls_before_unload >= initial_handler_calls + 2,
                 "deterministic in-flight handler was not observed")) {
        return 1;
    }

    (void)::CloseHandle(entered);
    (void)::CloseHandle(release);
    ::FreeLibrary(consumer);
    // The shim deliberately keeps the original provider function pointer for
    // process lifetime. Keep the synthetic provider loaded until process exit to
    // model ucrtbase.dll/other allocator providers in the real BDS process.
    (void)provider;
    return 0;
}
