#include "native/alloc/windows_allocation_shim_api.h"

#ifndef _WIN32
#error "windows_allocation_shim.cpp must only be compiled on Windows"
#endif

#include <cstdint>

#include "native/alloc/windows_callback_gate.h"

namespace {

spark::WindowsCallbackLifetimeGate GGate;
spark::WindowsAllocationShimTable GOriginals{};
spark::WindowsAllocationShimTable GHandlers{};

bool validOriginals(const spark::WindowsAllocationShimTable &table) noexcept
{
    return table.malloc_fn != nullptr && table.calloc_fn != nullptr && table.realloc_fn != nullptr &&
           table.free_fn != nullptr && table.aligned_free_fn != nullptr && table.heap_free_fn != nullptr;
}

template <typename Function>
Function chooseHandler(Function handler, Function original) noexcept
{
    return handler != nullptr ? handler : original;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimPin() noexcept
{
    HMODULE module = nullptr;
    LPCWSTR address = reinterpret_cast<LPCWSTR>(&sparkAllocationShimPin);
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    return ::GetModuleHandleExW(flags, address, &module) != FALSE ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimConfigure(
    const spark::WindowsAllocationShimTable *originals) noexcept
{
    if (originals == nullptr || !GGate.drained() || !validOriginals(*originals)) {
        return 0;
    }
    GOriginals = *originals;
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimActivate(
    const spark::WindowsAllocationShimTable *handlers) noexcept
{
    if (handlers == nullptr || !GGate.drained()) {
        return 0;
    }
    GHandlers = *handlers;
    if (!GGate.open()) {
        GHandlers = {};
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimBeginDeactivate() noexcept
{
    return GGate.close() ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimDrained() noexcept
{
    return GGate.drained() ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimFinishDeactivate() noexcept
{
    if (!GGate.drained()) {
        return 0;
    }
    GHandlers = {};
    return 1;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimMalloc(std::size_t size) noexcept
{
    spark::WindowsMallocFn original = GOriginals.malloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(size);
    }
    spark::WindowsMallocFn target = chooseHandler(GHandlers.malloc_fn, original);
    void *result = target(size);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimCalloc(std::size_t count, std::size_t size) noexcept
{
    spark::WindowsCallocFn original = GOriginals.calloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(count, size);
    }
    spark::WindowsCallocFn target = chooseHandler(GHandlers.calloc_fn, original);
    void *result = target(count, size);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimRealloc(void *pointer, std::size_t size) noexcept
{
    spark::WindowsReallocFn original = GOriginals.realloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(pointer, size);
    }
    spark::WindowsReallocFn target = chooseHandler(GHandlers.realloc_fn, original);
    void *result = target(pointer, size);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimRecalloc(void *pointer, std::size_t count,
                                                                           std::size_t size) noexcept
{
    spark::WindowsRecallocFn original = GOriginals.recalloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(pointer, count, size);
    }
    spark::WindowsRecallocFn target = chooseHandler(GHandlers.recalloc_fn, original);
    void *result = target(pointer, count, size);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void __cdecl sparkAllocationShimFree(void *pointer) noexcept
{
    spark::WindowsFreeFn original = GOriginals.free_fn;
    if (original == nullptr) {
        return;
    }
    if (!GGate.tryEnter()) {
        original(pointer);
        return;
    }
    spark::WindowsFreeFn target = chooseHandler(GHandlers.free_fn, original);
    target(pointer);
    (void)GGate.leave();
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedMalloc(std::size_t size,
                                                                                std::size_t alignment) noexcept
{
    spark::WindowsAlignedMallocFn original = GOriginals.aligned_malloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(size, alignment);
    }
    spark::WindowsAlignedMallocFn target = chooseHandler(GHandlers.aligned_malloc_fn, original);
    void *result = target(size, alignment);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedRealloc(void *pointer, std::size_t size,
                                                                                 std::size_t alignment) noexcept
{
    spark::WindowsAlignedReallocFn original = GOriginals.aligned_realloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(pointer, size, alignment);
    }
    spark::WindowsAlignedReallocFn target = chooseHandler(GHandlers.aligned_realloc_fn, original);
    void *result = target(pointer, size, alignment);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedRecalloc(void *pointer, std::size_t count,
                                                                                  std::size_t size,
                                                                                  std::size_t alignment) noexcept
{
    spark::WindowsAlignedRecallocFn original = GOriginals.aligned_recalloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(pointer, count, size, alignment);
    }
    spark::WindowsAlignedRecallocFn target = chooseHandler(GHandlers.aligned_recalloc_fn, original);
    void *result = target(pointer, count, size, alignment);
    (void)GGate.leave();
    return result;
}

extern "C"
    __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedOffsetMalloc(std::size_t size, std::size_t alignment,
                                                                               std::size_t offset) noexcept
{
    spark::WindowsAlignedOffsetMallocFn original = GOriginals.aligned_offset_malloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(size, alignment, offset);
    }
    spark::WindowsAlignedOffsetMallocFn target = chooseHandler(GHandlers.aligned_offset_malloc_fn, original);
    void *result = target(size, alignment, offset);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedOffsetRealloc(void *pointer, std::size_t size,
                                                                                       std::size_t alignment,
                                                                                       std::size_t offset) noexcept
{
    spark::WindowsAlignedOffsetReallocFn original = GOriginals.aligned_offset_realloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(pointer, size, alignment, offset);
    }
    spark::WindowsAlignedOffsetReallocFn target = chooseHandler(GHandlers.aligned_offset_realloc_fn, original);
    void *result = target(pointer, size, alignment, offset);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedOffsetRecalloc(
    void *pointer, std::size_t count, std::size_t size, std::size_t alignment, std::size_t offset) noexcept
{
    spark::WindowsAlignedOffsetRecallocFn original = GOriginals.aligned_offset_recalloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(pointer, count, size, alignment, offset);
    }
    spark::WindowsAlignedOffsetRecallocFn target = chooseHandler(GHandlers.aligned_offset_recalloc_fn, original);
    void *result = target(pointer, count, size, alignment, offset);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void __cdecl sparkAllocationShimAlignedFree(void *pointer) noexcept
{
    spark::WindowsFreeFn original = GOriginals.aligned_free_fn;
    if (original == nullptr) {
        return;
    }
    if (!GGate.tryEnter()) {
        original(pointer);
        return;
    }
    spark::WindowsFreeFn target = chooseHandler(GHandlers.aligned_free_fn, original);
    target(pointer);
    (void)GGate.leave();
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimMallocBase(std::size_t size) noexcept
{
    spark::WindowsMallocFn original = GOriginals.malloc_base_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(size);
    }
    spark::WindowsMallocFn target = chooseHandler(GHandlers.malloc_base_fn, original);
    void *result = target(size);
    (void)GGate.leave();
    return result;
}

extern "C"
    __declspec(dllexport) void *__cdecl sparkAllocationShimCallocBase(std::size_t count, std::size_t size) noexcept
{
    spark::WindowsCallocFn original = GOriginals.calloc_base_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(count, size);
    }
    spark::WindowsCallocFn target = chooseHandler(GHandlers.calloc_base_fn, original);
    void *result = target(count, size);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimReallocBase(void *pointer, std::size_t size) noexcept
{
    spark::WindowsReallocFn original = GOriginals.realloc_base_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(pointer, size);
    }
    spark::WindowsReallocFn target = chooseHandler(GHandlers.realloc_base_fn, original);
    void *result = target(pointer, size);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void __cdecl sparkAllocationShimFreeBase(void *pointer) noexcept
{
    spark::WindowsFreeFn original = GOriginals.free_base_fn;
    if (original == nullptr) {
        return;
    }
    if (!GGate.tryEnter()) {
        original(pointer);
        return;
    }
    spark::WindowsFreeFn target = chooseHandler(GHandlers.free_base_fn, original);
    target(pointer);
    (void)GGate.leave();
}

extern "C" __declspec(dllexport) void *WINAPI sparkAllocationShimHeapAlloc(HANDLE heap, DWORD flags,
                                                                           SIZE_T size) noexcept
{
    spark::WindowsHeapAllocFn original = GOriginals.heap_alloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(heap, flags, size);
    }
    spark::WindowsHeapAllocFn target = chooseHandler(GHandlers.heap_alloc_fn, original);
    void *result = target(heap, flags, size);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *WINAPI sparkAllocationShimHeapReAlloc(HANDLE heap, DWORD flags, void *pointer,
                                                                             SIZE_T size) noexcept
{
    spark::WindowsHeapReAllocFn original = GOriginals.heap_realloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!GGate.tryEnter()) {
        return original(heap, flags, pointer, size);
    }
    spark::WindowsHeapReAllocFn target = chooseHandler(GHandlers.heap_realloc_fn, original);
    void *result = target(heap, flags, pointer, size);
    (void)GGate.leave();
    return result;
}

extern "C" __declspec(dllexport) BOOL WINAPI sparkAllocationShimHeapFree(HANDLE heap, DWORD flags,
                                                                         void *pointer) noexcept
{
    spark::WindowsHeapFreeFn original = GOriginals.heap_free_fn;
    if (original == nullptr) {
        return FALSE;
    }
    if (!GGate.tryEnter()) {
        return original(heap, flags, pointer);
    }
    spark::WindowsHeapFreeFn target = chooseHandler(GHandlers.heap_free_fn, original);
    const BOOL result = target(heap, flags, pointer);
    (void)GGate.leave();
    return result;
}
