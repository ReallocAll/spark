#include <funchook.h>

#ifndef _WIN32
#error "windows_iat_funchook_compat.cpp must only be compiled on Windows"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native/alloc/windows_allocation_shim_api.h"
#include "native/alloc/windows_iat_hooks.h"

namespace {

constexpr const wchar_t *KShimFileName = L"spark_allocation_shim.dll";
constexpr std::uint64_t KShimDrainTimeoutMs = 5000;

struct HookRecord {
    const char *name = nullptr;
    const char *shim_export = nullptr;
    void *original = nullptr;
    void *handler = nullptr;
    bool required_coverage = false;
};

struct TargetDescription {
    const char *name = nullptr;
    const char *shim_export = nullptr;
    bool required_coverage = false;
};

struct ShimApi {
    HMODULE module = nullptr;
    spark::WindowsAllocationShimPinFn pin = nullptr;
    spark::WindowsAllocationShimConfigureFn configure = nullptr;
    spark::WindowsAllocationShimActivateFn activate = nullptr;
    spark::WindowsAllocationShimBeginDeactivateFn begin_deactivate = nullptr;
    spark::WindowsAllocationShimDrainedFn drained = nullptr;
    spark::WindowsAllocationShimFinishDeactivateFn finish_deactivate = nullptr;
};

template <typename Function>
Function proc(HMODULE module, const char *name) noexcept
{
    return reinterpret_cast<Function>(::GetProcAddress(module, name));
}

bool sameExport(HMODULE module, const char *name, void *address) noexcept
{
    return module != nullptr && ::GetProcAddress(module, name) == address;
}

bool describeTarget(void *address, TargetDescription &description) noexcept
{
    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const struct Candidate {
        HMODULE module;
        const char *name;
        const char *shim_export;
        bool required_coverage;
    } candidates[] = {
        {.module = ucrt, .name = "malloc", .shim_export = "sparkAllocationShimMalloc", .required_coverage = true},
        {.module = ucrt, .name = "calloc", .shim_export = "sparkAllocationShimCalloc", .required_coverage = false},
        {.module = ucrt, .name = "realloc", .shim_export = "sparkAllocationShimRealloc", .required_coverage = false},
        {.module = ucrt, .name = "_recalloc", .shim_export = "sparkAllocationShimRecalloc", .required_coverage = false},
        {.module = ucrt, .name = "free", .shim_export = "sparkAllocationShimFree", .required_coverage = true},
        {.module = ucrt,
         .name = "_aligned_malloc",
         .shim_export = "sparkAllocationShimAlignedMalloc",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_aligned_realloc",
         .shim_export = "sparkAllocationShimAlignedRealloc",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_aligned_recalloc",
         .shim_export = "sparkAllocationShimAlignedRecalloc",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_aligned_offset_malloc",
         .shim_export = "sparkAllocationShimAlignedOffsetMalloc",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_aligned_offset_realloc",
         .shim_export = "sparkAllocationShimAlignedOffsetRealloc",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_aligned_offset_recalloc",
         .shim_export = "sparkAllocationShimAlignedOffsetRecalloc",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_aligned_free",
         .shim_export = "sparkAllocationShimAlignedFree",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_malloc_base",
         .shim_export = "sparkAllocationShimMallocBase",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_calloc_base",
         .shim_export = "sparkAllocationShimCallocBase",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_realloc_base",
         .shim_export = "sparkAllocationShimReallocBase",
         .required_coverage = false},
        {.module = ucrt,
         .name = "_free_base",
         .shim_export = "sparkAllocationShimFreeBase",
         .required_coverage = false},
        {.module = kernel32,
         .name = "HeapAlloc",
         .shim_export = "sparkAllocationShimHeapAlloc",
         .required_coverage = false},
        {.module = kernel32,
         .name = "HeapReAlloc",
         .shim_export = "sparkAllocationShimHeapReAlloc",
         .required_coverage = false},
        {.module = kernel32,
         .name = "HeapFree",
         .shim_export = "sparkAllocationShimHeapFree",
         .required_coverage = false},
    };
    for (const Candidate &candidate : candidates) {
        if (sameExport(candidate.module, candidate.name, address)) {
            description = {.name = candidate.name,
                           .shim_export = candidate.shim_export,
                           .required_coverage = candidate.required_coverage};
            return true;
        }
    }
    return false;
}

bool moduleDirectory(std::wstring &directory, std::string &error)
{
    HMODULE owner = nullptr;
    const auto *const address = reinterpret_cast<LPCWSTR>(&moduleDirectory);
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             address, &owner) == FALSE) {
        error = "GetModuleHandleExW for Spark allocation hook module failed: " + std::to_string(::GetLastError());
        return false;
    }

    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || static_cast<std::size_t>(length) >= path.size()) {
        error = "GetModuleFileNameW for Spark allocation hook module failed: " + std::to_string(::GetLastError());
        return false;
    }
    path.resize(length);
    const std::size_t separator = path.find_last_of(L"/\\");
    if (separator == std::wstring::npos) {
        error = "Spark allocation hook module has no containing directory";
        return false;
    }
    directory.assign(path.data(), separator + 1);
    return true;
}

bool loadShim(ShimApi &api, std::string &error)
{
    if (api.module != nullptr) {
        return true;
    }
    std::wstring directory;
    if (!moduleDirectory(directory, error)) {
        return false;
    }
    const std::wstring primary_path = directory + KShimFileName;
    api.module = ::LoadLibraryW(primary_path.c_str());
    if (api.module == nullptr) {
        const DWORD primary_error = ::GetLastError();

        // Endstone shadow-copies C++ plugins into plugins/.local before
        // LoadLibrary so hot reload can replace the original plugin file.
        // The process-lifetime allocation shim remains next to the original
        // plugin in plugins/, therefore a module-relative lookup from the
        // loaded shadow copy must explicitly fall back one level.
        std::wstring module_directory = directory;
        while (!module_directory.empty() &&
               (module_directory.back() == L'\\' || module_directory.back() == L'/')) {
            module_directory.pop_back();
        }
        const std::size_t separator = module_directory.find_last_of(L"/\\");
        const std::wstring leaf = separator == std::wstring::npos
                                      ? module_directory
                                      : module_directory.substr(separator + 1);
        if (leaf != L".local" || separator == std::wstring::npos) {
            error = "LoadLibraryW(spark_allocation_shim.dll) failed from plugin module directory: " +
                    std::to_string(primary_error);
            return false;
        }

        const std::wstring fallback_path =
            module_directory.substr(0, separator + 1) + KShimFileName;
        api.module = ::LoadLibraryW(fallback_path.c_str());
        if (api.module == nullptr) {
            error = "LoadLibraryW(spark_allocation_shim.dll) failed from Endstone shadow-copy directory: " +
                    std::to_string(primary_error) + "; plugins parent fallback failed: " +
                    std::to_string(::GetLastError());
            return false;
        }
    }


    api.pin = proc<spark::WindowsAllocationShimPinFn>(api.module, "sparkAllocationShimPin");
    api.configure = proc<spark::WindowsAllocationShimConfigureFn>(api.module, "sparkAllocationShimConfigure");
    api.activate = proc<spark::WindowsAllocationShimActivateFn>(api.module, "sparkAllocationShimActivate");
    api.begin_deactivate =
        proc<spark::WindowsAllocationShimBeginDeactivateFn>(api.module, "sparkAllocationShimBeginDeactivate");
    api.drained = proc<spark::WindowsAllocationShimDrainedFn>(api.module, "sparkAllocationShimDrained");
    api.finish_deactivate =
        proc<spark::WindowsAllocationShimFinishDeactivateFn>(api.module, "sparkAllocationShimFinishDeactivate");
    if (api.pin == nullptr || api.configure == nullptr || api.activate == nullptr || api.begin_deactivate == nullptr ||
        api.drained == nullptr || api.finish_deactivate == nullptr) {
        error = "spark_allocation_shim.dll is missing required lifecycle exports";
        ::FreeLibrary(api.module);
        api = {};
        return false;
    }
    if (api.pin() == 0) {
        error = "spark_allocation_shim.dll could not pin itself for process lifetime";
        ::FreeLibrary(api.module);
        api = {};
        return false;
    }
    return true;
}

void setTableEntry(spark::WindowsAllocationShimTable &table, const char *name, void *address) noexcept
{
    if (std::strcmp(name, "malloc") == 0) {
        table.malloc_fn = reinterpret_cast<spark::WindowsMallocFn>(address);
    }
    else if (std::strcmp(name, "calloc") == 0) {
        table.calloc_fn = reinterpret_cast<spark::WindowsCallocFn>(address);
    }
    else if (std::strcmp(name, "realloc") == 0) {
        table.realloc_fn = reinterpret_cast<spark::WindowsReallocFn>(address);
    }
    else if (std::strcmp(name, "_recalloc") == 0) {
        table.recalloc_fn = reinterpret_cast<spark::WindowsRecallocFn>(address);
    }
    else if (std::strcmp(name, "free") == 0) {
        table.free_fn = reinterpret_cast<spark::WindowsFreeFn>(address);
    }
    else if (std::strcmp(name, "_aligned_malloc") == 0) {
        table.aligned_malloc_fn = reinterpret_cast<spark::WindowsAlignedMallocFn>(address);
    }
    else if (std::strcmp(name, "_aligned_realloc") == 0) {
        table.aligned_realloc_fn = reinterpret_cast<spark::WindowsAlignedReallocFn>(address);
    }
    else if (std::strcmp(name, "_aligned_recalloc") == 0) {
        table.aligned_recalloc_fn = reinterpret_cast<spark::WindowsAlignedRecallocFn>(address);
    }
    else if (std::strcmp(name, "_aligned_offset_malloc") == 0) {
        table.aligned_offset_malloc_fn = reinterpret_cast<spark::WindowsAlignedOffsetMallocFn>(address);
    }
    else if (std::strcmp(name, "_aligned_offset_realloc") == 0) {
        table.aligned_offset_realloc_fn = reinterpret_cast<spark::WindowsAlignedOffsetReallocFn>(address);
    }
    else if (std::strcmp(name, "_aligned_offset_recalloc") == 0) {
        table.aligned_offset_recalloc_fn = reinterpret_cast<spark::WindowsAlignedOffsetRecallocFn>(address);
    }
    else if (std::strcmp(name, "_aligned_free") == 0) {
        table.aligned_free_fn = reinterpret_cast<spark::WindowsFreeFn>(address);
    }
    else if (std::strcmp(name, "_malloc_base") == 0) {
        table.malloc_base_fn = reinterpret_cast<spark::WindowsMallocFn>(address);
    }
    else if (std::strcmp(name, "_calloc_base") == 0) {
        table.calloc_base_fn = reinterpret_cast<spark::WindowsCallocFn>(address);
    }
    else if (std::strcmp(name, "_realloc_base") == 0) {
        table.realloc_base_fn = reinterpret_cast<spark::WindowsReallocFn>(address);
    }
    else if (std::strcmp(name, "_free_base") == 0) {
        table.free_base_fn = reinterpret_cast<spark::WindowsFreeFn>(address);
    }
    else if (std::strcmp(name, "HeapAlloc") == 0) {
        table.heap_alloc_fn = reinterpret_cast<spark::WindowsHeapAllocFn>(address);
    }
    else if (std::strcmp(name, "HeapReAlloc") == 0) {
        table.heap_realloc_fn = reinterpret_cast<spark::WindowsHeapReAllocFn>(address);
    }
    else if (std::strcmp(name, "HeapFree") == 0) {
        table.heap_free_fn = reinterpret_cast<spark::WindowsHeapFreeFn>(address);
    }
}

bool deactivateShim(ShimApi &api, std::string &error) noexcept
{
    if (api.module == nullptr) {
        return true;
    }
    if (api.begin_deactivate() == 0) {
        try {
            error = "Windows allocation shim callback gate could not close";
        }
        catch (...) {
            error.clear();
        }
        return false;
    }
    const std::uint64_t deadline = ::GetTickCount64() + KShimDrainTimeoutMs;
    while (api.drained() == 0) {
        if (::GetTickCount64() >= deadline) {
            try {
                error = "timed out waiting for Windows allocation shim callbacks to drain";
            }
            catch (...) {
                error.clear();
            }
            return false;
        }
        ::Sleep(1);
    }
    if (api.finish_deactivate() == 0) {
        try {
            error = "Windows allocation shim could not clear plugin handlers after drain";
        }
        catch (...) {
            error.clear();
        }
        return false;
    }
    return true;
}

}  // namespace

struct funchook {
    std::vector<HookRecord> records;
    std::unique_ptr<spark::WindowsIatHooks> hooks;
    ShimApi shim;
    bool installed = false;
    std::string error;
};

extern "C" funchook_t *funchook_create(void)
{
    try {
        return new funchook_t{};
    }
    catch (...) {
        return nullptr;
    }
}

extern "C" int funchook_prepare(funchook_t *funchook, void **target_func, void *hook_func)
{
    if (funchook == nullptr || target_func == nullptr || *target_func == nullptr || hook_func == nullptr) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    try {
        TargetDescription description;
        if (!describeTarget(*target_func, description)) {
            funchook->error = "unsupported Windows allocation hook target";
            return FUNCHOOK_ERROR_INTERNAL;
        }
        funchook->records.push_back({.name = description.name,
                                     .shim_export = description.shim_export,
                                     .original = *target_func,
                                     .handler = hook_func,
                                     .required_coverage = description.required_coverage});
        return FUNCHOOK_ERROR_SUCCESS;
    }
    catch (...) {
        funchook->error = "could not prepare Windows IAT allocation hook target";
        return FUNCHOOK_ERROR_INTERNAL;
    }
}

extern "C" int funchook_install(funchook_t *funchook, int flags)
{
    (void)flags;
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    if (funchook->installed) {
        return FUNCHOOK_ERROR_SUCCESS;
    }
    try {
        funchook->error.clear();
        if (funchook->records.empty()) {
            funchook->error = "Windows IAT allocation hook target list is empty";
            return FUNCHOOK_ERROR_INTERNAL;
        }
        if (!loadShim(funchook->shim, funchook->error)) {
            return FUNCHOOK_ERROR_INTERNAL;
        }

        spark::WindowsAllocationShimTable originals{};
        spark::WindowsAllocationShimTable handlers{};
        std::vector<spark::WindowsIatHookTarget> targets;
        targets.reserve(funchook->records.size());
        for (const HookRecord &record : funchook->records) {
            setTableEntry(originals, record.name, record.original);
            setTableEntry(handlers, record.name, record.handler);
            void *replacement = reinterpret_cast<void *>(::GetProcAddress(funchook->shim.module, record.shim_export));
            if (replacement == nullptr) {
                funchook->error =
                    std::string("spark_allocation_shim.dll is missing hook export: ") + record.shim_export;
                return FUNCHOOK_ERROR_INTERNAL;
            }
            targets.push_back({.import_name = record.name,
                               .import_modules = {},
                               .original = record.original,
                               .replacement = replacement,
                               .required = record.required_coverage});
        }
        if (funchook->shim.configure(&originals) == 0) {
            funchook->error = "Windows allocation shim rejected the original allocator table";
            return FUNCHOOK_ERROR_INTERNAL;
        }

        auto backend = spark::makeNativeWindowsIatHookBackend(reinterpret_cast<void *>(&funchook_install));
        if (backend == nullptr) {
            funchook->error = "native Windows IAT hook backend is unavailable";
            return FUNCHOOK_ERROR_INTERNAL;
        }
        funchook->hooks = std::make_unique<spark::WindowsIatHooks>(std::move(backend));
        if (!funchook->hooks->configure(std::move(targets), funchook->error) ||
            !funchook->hooks->install(funchook->error)) {
            funchook->hooks.reset();
            return FUNCHOOK_ERROR_INTERNAL;
        }
        if (funchook->shim.activate(&handlers) == 0) {
            std::string uninstall_error;
            (void)funchook->hooks->uninstall(uninstall_error);
            funchook->hooks.reset();
            funchook->error = "Windows allocation shim could not publish Spark handlers";
            if (!uninstall_error.empty()) {
                funchook->error += "; IAT rollback: " + uninstall_error;
            }
            return FUNCHOOK_ERROR_INTERNAL;
        }
        funchook->installed = true;
        return FUNCHOOK_ERROR_SUCCESS;
    }
    catch (const std::exception &exception) {
        funchook->error = std::string("Windows IAT allocation hook install failed: ") + exception.what();
        return FUNCHOOK_ERROR_INTERNAL;
    }
    catch (...) {
        funchook->error = "Windows IAT allocation hook install failed";
        return FUNCHOOK_ERROR_INTERNAL;
    }
}

extern "C" int funchook_uninstall(funchook_t *funchook, int flags)
{
    (void)flags;
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    if (!funchook->installed) {
        return FUNCHOOK_ERROR_NOT_INSTALLED;
    }

    funchook->error.clear();
    if (!deactivateShim(funchook->shim, funchook->error)) {
        return FUNCHOOK_ERROR_INTERNAL;
    }

    // Once the process-lifetime shim is closed, drained, and has forgotten all
    // plugin handlers, stale or ownership-uncertain IAT entries are still safe:
    // they can only execute the pinned fallback path. Restoration is therefore
    // best-effort for cleanliness, not a prerequisite for unload safety.
    if (funchook->hooks != nullptr) {
        std::string detach_error;
        if (!funchook->hooks->uninstall(detach_error) && funchook->error.empty()) {
            funchook->error = "Windows allocation IAT detach left safe shim entries active: " + detach_error;
        }
    }
    funchook->installed = false;
    return FUNCHOOK_ERROR_SUCCESS;
}

extern "C" int funchook_destroy(funchook_t *funchook)
{
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_SUCCESS;
    }
    if (funchook->installed) {
        funchook->error = "Windows allocation hook context is still installed";
        return FUNCHOOK_ERROR_INTERNAL;
    }
    if (funchook->shim.module != nullptr) {
        ::FreeLibrary(funchook->shim.module);
        funchook->shim.module = nullptr;
    }
    delete funchook;
    return FUNCHOOK_ERROR_SUCCESS;
}

extern "C" const char *funchook_error_message(funchook_t *funchook)
{
    return funchook == nullptr ? "Windows IAT allocation hook context is null" : funchook->error.c_str();
}
