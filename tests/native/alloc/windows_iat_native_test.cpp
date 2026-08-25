#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

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

bool concurrentInstallUninstallStress(spark::WindowsIatHooks &hooks, HMODULE consumer, std::string &error)
{
    constexpr int KWorkers = 8;
    constexpr int KCycles = 1000;

    ConsumerAllocFn allocate = function<ConsumerAllocFn>(consumer, "sparkIatConsumerAlloc");
    ConsumerFreeFn release = function<ConsumerFreeFn>(consumer, "sparkIatConsumerFree");
    if (!require(allocate != nullptr && release != nullptr, "stress consumer exports are unavailable")) {
        return false;
    }

    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> successful_allocations{0};
    std::atomic<std::uint64_t> failed_allocations{0};
    std::vector<std::thread> workers;
    workers.reserve(KWorkers);
    for (int index = 0; index < KWorkers; ++index) {
        workers.emplace_back([&] {
            while (running.load(std::memory_order_acquire)) {
                void *memory = allocate(32 + static_cast<std::size_t>(index));
                if (memory == nullptr) {
                    failed_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                release(memory);
                successful_allocations.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    bool ok = true;
    for (int cycle = 0; cycle < KCycles; ++cycle) {
        if (!hooks.uninstall(error) || hooks.installed() || hooks.unsafeState()) {
            ok = false;
            break;
        }
        if (!hooks.install(error) || !hooks.installed() || hooks.unsafeState()) {
            ok = false;
            break;
        }
    }

    running.store(false, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }

    return require(ok, error.empty() ? "concurrent install/uninstall stress failed" : error.c_str()) &&
           require(failed_allocations.load(std::memory_order_relaxed) == 0,
                   "allocator callback failed during install/uninstall stress") &&
           require(successful_allocations.load(std::memory_order_relaxed) > 0,
                   "allocator workers made no progress during install/uninstall stress");
}

bool moduleReloadStress(spark::WindowsIatHooks &hooks, HMODULE &consumer, std::string &error)
{
    constexpr int KCycles = 500;
    for (int cycle = 0; cycle < KCycles; ++cycle) {
        if (!require(::FreeLibrary(consumer) != FALSE, "consumer FreeLibrary failed during reload stress")) {
            consumer = nullptr;
            return false;
        }
        consumer = load(L".\\windows_iat_consumer.dll");
        if (consumer == nullptr ||
            !require(hooks.refresh(error), error.empty() ? "refresh during reload stress failed" : error.c_str()) ||
            !require(hooks.activeSlotCount() == 1, "reload stress did not converge to one owned slot") ||
            !exerciseConsumer(consumer, 1)) {
            return false;
        }
    }
    return true;
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
        !exerciseConsumer(consumer, 1) || !concurrentInstallUninstallStress(hooks, consumer, error)) {
        std::string ignored;
        (void)hooks.uninstall(ignored);
        ::FreeLibrary(consumer);
        ::FreeLibrary(provider);
        return 1;
    }

    // Repeatedly unload and reload the consumer while the old slot is still
    // registered. refresh() must treat each old mapping as stale (including
    // same-base address reuse), discover the new import slot, and never
    // dereference freed memory.
    if (!moduleReloadStress(hooks, consumer, error)) {
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
