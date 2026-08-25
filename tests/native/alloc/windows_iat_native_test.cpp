#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

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

AllocFn gOriginalAlloc = nullptr;
std::atomic<std::uint64_t> gHookCalls{0};

void *__cdecl hookProviderAlloc(std::size_t size) noexcept
{
    gHookCalls.fetch_add(1, std::memory_order_relaxed);
    return gOriginalAlloc(size);
}

bool require(bool condition, const char *message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "native Windows IAT hooks: %s\n", message);
    return false;
}

HMODULE load(const wchar_t *name)
{
    HMODULE module = ::LoadLibraryW(name);
    if (module == nullptr) {
        std::fprintf(stderr, "native Windows IAT hooks: LoadLibraryW failed for %ls: %lu\n", name,
                     static_cast<unsigned long>(::GetLastError()));
    }
    return module;
}

template <typename Function>
Function function(HMODULE module, const char *name)
{
    return reinterpret_cast<Function>(::GetProcAddress(module, name));
}

bool exerciseConsumer(HMODULE consumer, std::uint64_t expected_increment)
{
    ConsumerAllocFn allocate = function<ConsumerAllocFn>(consumer, "sparkIatConsumerAlloc");
    ConsumerFreeFn release = function<ConsumerFreeFn>(consumer, "sparkIatConsumerFree");
    if (!require(allocate != nullptr && release != nullptr, "consumer exports are unavailable")) {
        return false;
    }

    const std::uint64_t before = gHookCalls.load(std::memory_order_relaxed);
    void *memory = allocate(64);
    if (!require(memory != nullptr, "consumer allocation failed")) {
        return false;
    }
    release(memory);
    const std::uint64_t after = gHookCalls.load(std::memory_order_relaxed);
    return require(after == before + expected_increment, "unexpected hook invocation count");
}

}  // namespace

int main()
{
    HMODULE provider = load(L".\\windows_iat_provider.dll");
    HMODULE consumer = load(L".\\windows_iat_consumer.dll");
    if (provider == nullptr || consumer == nullptr) {
        if (consumer != nullptr) {
            ::FreeLibrary(consumer);
        }
        if (provider != nullptr) {
            ::FreeLibrary(provider);
        }
        return 1;
    }

    gOriginalAlloc = function<AllocFn>(provider, "sparkIatProviderAlloc");
    if (!require(gOriginalAlloc != nullptr, "provider allocation export is unavailable")) {
        ::FreeLibrary(consumer);
        ::FreeLibrary(provider);
        return 1;
    }

    spark::WindowsIatHooks hooks(spark::makeNativeWindowsIatHookBackend(reinterpret_cast<void *>(&hookProviderAlloc)));
    spark::WindowsIatHookTarget target;
    target.import_name = "sparkIatProviderAlloc";
    target.import_modules = {"windows_iat_provider.dll"};
    target.original = reinterpret_cast<void *>(gOriginalAlloc);
    target.replacement = reinterpret_cast<void *>(&hookProviderAlloc);
    target.required = true;

    std::string error;
    if (!require(hooks.configure({target}, error), "configure failed") ||
        !require(hooks.install(error), error.empty() ? "install failed" : error.c_str()) ||
        !require(hooks.activeSlotCount() == 1, "fixture should own exactly one import slot") ||
        !exerciseConsumer(consumer, 1)) {
        std::string ignored;
        (void)hooks.uninstall(ignored);
        ::FreeLibrary(consumer);
        ::FreeLibrary(provider);
        return 1;
    }

    // Unload and reload the consumer while the old slot is still registered.
    // refresh() must treat the old mapping as stale (or detached at the same
    // reused base), discover the new import slot, and never dereference freed memory.
    ::FreeLibrary(consumer);
    consumer = load(L".\\windows_iat_consumer.dll");
    if (consumer == nullptr || !require(hooks.refresh(error), error.empty() ? "refresh after reload failed" : error.c_str()) ||
        !require(hooks.activeSlotCount() == 1, "reload should converge to one owned slot") ||
        !exerciseConsumer(consumer, 1)) {
        std::string ignored;
        (void)hooks.uninstall(ignored);
        if (consumer != nullptr) {
            ::FreeLibrary(consumer);
        }
        ::FreeLibrary(provider);
        return 1;
    }

    if (!require(hooks.uninstall(error), error.empty() ? "uninstall failed" : error.c_str()) ||
        !require(!hooks.installed() && !hooks.unsafeState(), "uninstall did not prove a clean detach") ||
        !exerciseConsumer(consumer, 0)) {
        ::FreeLibrary(consumer);
        ::FreeLibrary(provider);
        return 1;
    }

    ::FreeLibrary(consumer);
    ::FreeLibrary(provider);
    return 0;
}
