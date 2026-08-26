#ifndef ENDSTONE_SPARK_WINDOWS_ALLOCATION_SHIM_API_H
#define ENDSTONE_SPARK_WINDOWS_ALLOCATION_SHIM_API_H

#ifndef _WIN32
#error "windows_allocation_shim_api.h must only be used on Windows"
#endif

#include <cstddef>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace spark {

using WindowsMallocFn = void *(__cdecl *)(std::size_t);
using WindowsCallocFn = void *(__cdecl *)(std::size_t, std::size_t);
using WindowsReallocFn = void *(__cdecl *)(void *, std::size_t);
using WindowsRecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t);
using WindowsFreeFn = void(__cdecl *)(void *);
using WindowsAlignedMallocFn = void *(__cdecl *)(std::size_t, std::size_t);
using WindowsAlignedReallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t);
using WindowsAlignedRecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t);
using WindowsAlignedOffsetMallocFn = void *(__cdecl *)(std::size_t, std::size_t, std::size_t);
using WindowsAlignedOffsetReallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t);
using WindowsAlignedOffsetRecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t, std::size_t);
using WindowsHeapAllocFn = void *(WINAPI *)(HANDLE, DWORD, SIZE_T);
using WindowsHeapReAllocFn = void *(WINAPI *)(HANDLE, DWORD, void *, SIZE_T);
using WindowsHeapFreeFn = BOOL(WINAPI *)(HANDLE, DWORD, void *);

struct WindowsAllocationShimTable {
    WindowsMallocFn malloc_fn = nullptr;
    WindowsCallocFn calloc_fn = nullptr;
    WindowsReallocFn realloc_fn = nullptr;
    WindowsRecallocFn recalloc_fn = nullptr;
    WindowsFreeFn free_fn = nullptr;
    WindowsAlignedMallocFn aligned_malloc_fn = nullptr;
    WindowsAlignedReallocFn aligned_realloc_fn = nullptr;
    WindowsAlignedRecallocFn aligned_recalloc_fn = nullptr;
    WindowsAlignedOffsetMallocFn aligned_offset_malloc_fn = nullptr;
    WindowsAlignedOffsetReallocFn aligned_offset_realloc_fn = nullptr;
    WindowsAlignedOffsetRecallocFn aligned_offset_recalloc_fn = nullptr;
    WindowsFreeFn aligned_free_fn = nullptr;
    WindowsMallocFn malloc_base_fn = nullptr;
    WindowsCallocFn calloc_base_fn = nullptr;
    WindowsReallocFn realloc_base_fn = nullptr;
    WindowsFreeFn free_base_fn = nullptr;
    WindowsHeapAllocFn heap_alloc_fn = nullptr;
    WindowsHeapReAllocFn heap_realloc_fn = nullptr;
    WindowsHeapFreeFn heap_free_fn = nullptr;
};

using WindowsAllocationShimPinFn = int(__cdecl *)() noexcept;
using WindowsAllocationShimConfigureFn = int(__cdecl *)(const WindowsAllocationShimTable *) noexcept;
using WindowsAllocationShimActivateFn = int(__cdecl *)(const WindowsAllocationShimTable *) noexcept;
using WindowsAllocationShimBeginDeactivateFn = int(__cdecl *)() noexcept;
using WindowsAllocationShimDrainedFn = int(__cdecl *)() noexcept;
using WindowsAllocationShimFinishDeactivateFn = int(__cdecl *)() noexcept;

}  // namespace spark

#endif  // ENDSTONE_SPARK_WINDOWS_ALLOCATION_SHIM_API_H
