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

bool oversizedImportDirectoryRefresh(spark::WindowsIatHooks &hooks, HMODULE consumer, std::string &error)
{
    auto *base = reinterpret_cast<std::byte *>(consumer);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (!require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew >= 0, "consumer DOS header is invalid")) {
        return false;
    }

    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + static_cast<std::uint32_t>(dos->e_lfanew));
    if (!require(nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC,
                 "consumer NT header is invalid")) {
        return false;
    }

    IMAGE_DATA_DIRECTORY &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!require(directory.VirtualAddress != 0 && directory.VirtualAddress < nt->OptionalHeader.SizeOfImage,
                 "consumer import directory is unavailable")) {
        return false;
    }

    DWORD old_protection = 0;
    if (!require(::VirtualProtect(&directory.Size, sizeof(directory.Size), PAGE_READWRITE, &old_protection) != FALSE,
                 "VirtualProtect failed while expanding import directory metadata")) {
        return false;
    }

    const DWORD original_size = directory.Size;
    directory.Size = nt->OptionalHeader.SizeOfImage;
    const bool refreshed = hooks.refresh(error);
    directory.Size = original_size;

    DWORD ignored = 0;
    const bool protection_restored =
        ::VirtualProtect(&directory.Size, sizeof(directory.Size), old_protection, &ignored) != FALSE;
    return require(protection_restored, "VirtualProtect failed while restoring import directory metadata") &&
           require(refreshed, error.empty() ? "refresh rejected oversized import directory metadata" : error.c_str()) &&
           require(hooks.activeSlotCount() == 1, "oversized import metadata refresh lost the owned slot") &&
           exerciseConsumer(consumer, 1);
}

bool outOfRangeImportDirectoryRefresh(spark::WindowsIatHooks &hooks, HMODULE provider, HMODULE consumer,
                                      std::string &error)
{
    auto *base = reinterpret_cast<std::byte *>(provider);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (!require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew >= 0, "provider DOS header is invalid")) {
        return false;
    }

    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + static_cast<std::uint32_t>(dos->e_lfanew));
    if (!require(nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC,
                 "provider NT header is invalid")) {
        return false;
    }

    IMAGE_DATA_DIRECTORY &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!require(directory.VirtualAddress != 0 && directory.VirtualAddress < nt->OptionalHeader.SizeOfImage,
                 "provider import directory is unavailable")) {
        return false;
    }

    DWORD old_protection = 0;
    if (!require(::VirtualProtect(&directory.VirtualAddress, sizeof(directory.VirtualAddress), PAGE_READWRITE,
                                  &old_protection) != FALSE,
                 "VirtualProtect failed while moving provider import directory metadata")) {
        return false;
    }

    const DWORD original_rva = directory.VirtualAddress;
    directory.VirtualAddress = nt->OptionalHeader.SizeOfImage;
    const bool refreshed = hooks.refresh(error);
    directory.VirtualAddress = original_rva;

    DWORD ignored = 0;
    const bool protection_restored = ::VirtualProtect(&directory.VirtualAddress, sizeof(directory.VirtualAddress),
                                                      old_protection, &ignored) != FALSE;
    return require(protection_restored, "VirtualProtect failed while restoring provider import directory metadata") &&
           require(refreshed, error.empty() ? "refresh rejected an unrelated unscannable module" : error.c_str()) &&
           require(hooks.activeSlotCount() == 1, "unscannable provider refresh lost the owned consumer slot") &&
           exerciseConsumer(consumer, 1);
}

bool unalignedIatModuleRefresh(spark::WindowsIatHooks &hooks, HMODULE consumer, std::string &error)
{
    constexpr wchar_t KCopyName[] = L".\\windows_iat_consumer_unaligned.dll";
    (void)::DeleteFileW(KCopyName);
    if (!require(::CopyFileW(L".\\windows_iat_consumer.dll", KCopyName, FALSE) != FALSE,
                 "CopyFileW failed for unaligned consumer fixture")) {
        return false;
    }

    HMODULE malformed = load(KCopyName);
    if (malformed == nullptr) {
        (void)::DeleteFileW(KCopyName);
        return false;
    }

    auto *base = reinterpret_cast<std::byte *>(malformed);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + static_cast<std::uint32_t>(dos->e_lfanew));
    IMAGE_DATA_DIRECTORY &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto *descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);

    IMAGE_IMPORT_DESCRIPTOR *target_descriptor = nullptr;
    const std::size_t capacity = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t index = 0; index < capacity; ++index) {
        IMAGE_IMPORT_DESCRIPTOR &descriptor = descriptors[index];
        if (descriptor.Name == 0 && descriptor.FirstThunk == 0 && descriptor.OriginalFirstThunk == 0) {
            break;
        }
        if (descriptor.Name == 0) {
            continue;
        }
        const char *provider_name = reinterpret_cast<const char *>(base + descriptor.Name);
        if (::_stricmp(provider_name, "windows_iat_provider.dll") == 0) {
            target_descriptor = &descriptor;
            break;
        }
    }

    bool ok = require(target_descriptor != nullptr, "unaligned consumer provider descriptor was not found");
    DWORD old_protection = 0;
    DWORD original_first_thunk = 0;
    if (ok) {
        ok = require(::VirtualProtect(&target_descriptor->FirstThunk, sizeof(target_descriptor->FirstThunk),
                                      PAGE_READWRITE, &old_protection) != FALSE,
                     "VirtualProtect failed while misaligning consumer FirstThunk");
    }
    if (ok) {
        original_first_thunk = target_descriptor->FirstThunk;
        target_descriptor->FirstThunk = original_first_thunk + 1;
        error.clear();
        ok = require(hooks.refresh(error),
                     error.empty() ? "refresh rejected unrelated unaligned IAT module" : error.c_str()) &&
             require(hooks.activeSlotCount() == 1, "unaligned module refresh changed owned-slot count") &&
             exerciseConsumer(consumer, 1);
        target_descriptor->FirstThunk = original_first_thunk;
        DWORD ignored = 0;
        ok = require(::VirtualProtect(&target_descriptor->FirstThunk, sizeof(target_descriptor->FirstThunk),
                                      old_protection, &ignored) != FALSE,
                     "VirtualProtect failed while restoring consumer FirstThunk protection") &&
             ok;
    }

    const bool unloaded = ::FreeLibrary(malformed) != FALSE;
    const bool deleted = ::DeleteFileW(KCopyName) != FALSE;
    return require(unloaded, "unaligned consumer FreeLibrary failed") &&
           require(deleted, "unaligned consumer fixture cleanup failed") && ok;
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
        workers.emplace_back([&, index] {
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
        !exerciseConsumer(consumer, 1) || !concurrentInstallUninstallStress(hooks, consumer, error) ||
        !oversizedImportDirectoryRefresh(hooks, consumer, error) ||
        !outOfRangeImportDirectoryRefresh(hooks, provider, consumer, error) ||
        !unalignedIatModuleRefresh(hooks, consumer, error)) {
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
