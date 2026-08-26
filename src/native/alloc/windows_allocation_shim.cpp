#include "native/alloc/windows_allocation_shim_api.h"

#ifndef _WIN32
#error "windows_allocation_shim.cpp must only be compiled on Windows"
#endif

#include <cstdint>

#include "native/alloc/windows_callback_gate.h"

namespace {

spark::WindowsCallbackLifetimeGate gGate;
spark::WindowsAllocationShimTable gOriginals{};
spark::WindowsAllocationShimTable gHandlers{};

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
    const auto address = reinterpret_cast<LPCWSTR>(reinterpret_cast<std::uintptr_t>(&sparkAllocationShimPin));
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    return ::GetModuleHandleExW(flags, address, &module) != FALSE ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl
sparkAllocationShimConfigure(const spark::WindowsAllocationShimTable *originals) noexcept
{
    if (originals == nullptr || !gGate.drained() || !validOriginals(*originals)) {
        return 0;
    }
    gOriginals = *originals;
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl
sparkAllocationShimActivate(const spark::WindowsAllocationShimTable *handlers) noexcept
{
    if (handlers == nullptr || !gGate.drained()) {
        return 0;
    }
    gHandlers = *handlers;
    if (!gGate.open()) {
        gHandlers = {};
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimBeginDeactivate() noexcept
{
    return gGate.close() ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimDrained() noexcept
{
    return gGate.drained() ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl sparkAllocationShimFinishDeactivate() noexcept
{
    if (!gGate.drained()) {
        return 0;
    }
    gHandlers = {};
    return 1;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimMalloc(std::size_t size) noexcept
{
    const spark::WindowsMallocFn original = gOriginals.malloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(size);
    }
    const spark::WindowsMallocFn target = chooseHandler(gHandlers.malloc_fn, original);
    void *result = target(size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimCalloc(std::size_t count, std::size_t size) noexcept
{
    const spark::WindowsCallocFn original = gOriginals.calloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(count, size);
    }
    const spark::WindowsCallocFn target = chooseHandler(gHandlers.calloc_fn, original);
    void *result = target(count, size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimRealloc(void *pointer, std::size_t size) noexcept
{
    const spark::WindowsReallocFn original = gOriginals.realloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(pointer, size);
    }
    const spark::WindowsReallocFn target = chooseHandler(gHandlers.realloc_fn, original);
    void *result = target(pointer, size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimRecalloc(void *pointer, std::size_t count,
                                                                            std::size_t size) noexcept
{
    const spark::WindowsRecallocFn original = gOriginals.recalloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(pointer, count, size);
    }
    const spark::WindowsRecallocFn target = chooseHandler(gHandlers.recalloc_fn, original);
    void *result = target(pointer, count, size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void __cdecl sparkAllocationShimFree(void *pointer) noexcept
{
    const spark::WindowsFreeFn original = gOriginals.free_fn;
    if (original == nullptr) {
        return;
    }
    if (!gGate.tryEnter()) {
        original(pointer);
        return;
    }
    const spark::WindowsFreeFn target = chooseHandler(gHandlers.free_fn, original);
    target(pointer);
    (void)gGate.leave();
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedMalloc(std::size_t size,
                                                                                 std::size_t alignment) noexcept
{
    const spark::WindowsAlignedMallocFn original = gOriginals.aligned_malloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(size, alignment);
    }
    const spark::WindowsAlignedMallocFn target = chooseHandler(gHandlers.aligned_malloc_fn, original);
    void *result = target(size, alignment);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedRealloc(void *pointer, std::size_t size,
                                                                                  std::size_t alignment) noexcept
{
    const spark::WindowsAlignedReallocFn original = gOriginals.aligned_realloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(pointer, size, alignment);
    }
    const spark::WindowsAlignedReallocFn target = chooseHandler(gHandlers.aligned_realloc_fn, original);
    void *result = target(pointer, size, alignment);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedRecalloc(void *pointer, std::size_t count,
                                                                                   std::size_t size,
                                                                                   std::size_t alignment) noexcept
{
    const spark::WindowsAlignedRecallocFn original = gOriginals.aligned_recalloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(pointer, count, size, alignment);
    }
    const spark::WindowsAlignedRecallocFn target = chooseHandler(gHandlers.aligned_recalloc_fn, original);
    void *result = target(pointer, count, size, alignment);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedOffsetMalloc(std::size_t size,
                                                                                       std::size_t alignment,
                                                                                       std::size_t offset) noexcept
{
    const spark::WindowsAlignedOffsetMallocFn original = gOriginals.aligned_offset_malloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(size, alignment, offset);
    }
    const spark::WindowsAlignedOffsetMallocFn target = chooseHandler(gHandlers.aligned_offset_malloc_fn, original);
    void *result = target(size, alignment, offset);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedOffsetRealloc(void *pointer,
                                                                                        std::size_t size,
                                                                                        std::size_t alignment,
                                                                                        std::size_t offset) noexcept
{
    const spark::WindowsAlignedOffsetReallocFn original = gOriginals.aligned_offset_realloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(pointer, size, alignment, offset);
    }
    const spark::WindowsAlignedOffsetReallocFn target = chooseHandler(gHandlers.aligned_offset_realloc_fn, original);
    void *result = target(pointer, size, alignment, offset);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimAlignedOffsetRecalloc(void *pointer,
                                                                                         std::size_t count,
                                                                                         std::size_t size,
                                                                                         std::size_t alignment,
                                                                                         std::size_t offset) noexcept
{
    const spark::WindowsAlignedOffsetRecallocFn original = gOriginals.aligned_offset_recalloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(pointer, count, size, alignment, offset);
    }
    const spark::WindowsAlignedOffsetRecallocFn target = chooseHandler(gHandlers.aligned_offset_recalloc_fn, original);
    void *result = target(pointer, count, size, alignment, offset);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void __cdecl sparkAllocationShimAlignedFree(void *pointer) noexcept
{
    const spark::WindowsFreeFn original = gOriginals.aligned_free_fn;
    if (original == nullptr) {
        return;
    }
    if (!gGate.tryEnter()) {
        original(pointer);
        return;
    }
    const spark::WindowsFreeFn target = chooseHandler(gHandlers.aligned_free_fn, original);
    target(pointer);
    (void)gGate.leave();
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimMallocBase(std::size_t size) noexcept
{
    const spark::WindowsMallocFn original = gOriginals.malloc_base_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(size);
    }
    const spark::WindowsMallocFn target = chooseHandler(gHandlers.malloc_base_fn, original);
    void *result = target(size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimCallocBase(std::size_t count,
                                                                              std::size_t size) noexcept
{
    const spark::WindowsCallocFn original = gOriginals.calloc_base_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(count, size);
    }
    const spark::WindowsCallocFn target = chooseHandler(gHandlers.calloc_base_fn, original);
    void *result = target(count, size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *__cdecl sparkAllocationShimReallocBase(void *pointer, std::size_t size) noexcept
{
    const spark::WindowsReallocFn original = gOriginals.realloc_base_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(pointer, size);
    }
    const spark::WindowsReallocFn target = chooseHandler(gHandlers.realloc_base_fn, original);
    void *result = target(pointer, size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void __cdecl sparkAllocationShimFreeBase(void *pointer) noexcept
{
    const spark::WindowsFreeFn original = gOriginals.free_base_fn;
    if (original == nullptr) {
        return;
    }
    if (!gGate.tryEnter()) {
        original(pointer);
        return;
    }
    const spark::WindowsFreeFn target = chooseHandler(gHandlers.free_base_fn, original);
    target(pointer);
    (void)gGate.leave();
}

extern "C" __declspec(dllexport) void *WINAPI sparkAllocationShimHeapAlloc(HANDLE heap, DWORD flags,
                                                                            SIZE_T size) noexcept
{
    const spark::WindowsHeapAllocFn original = gOriginals.heap_alloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(heap, flags, size);
    }
    const spark::WindowsHeapAllocFn target = chooseHandler(gHandlers.heap_alloc_fn, original);
    void *result = target(heap, flags, size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) void *WINAPI sparkAllocationShimHeapReAlloc(HANDLE heap, DWORD flags, void *pointer,
                                                                              SIZE_T size) noexcept
{
    const spark::WindowsHeapReAllocFn original = gOriginals.heap_realloc_fn;
    if (original == nullptr) {
        return nullptr;
    }
    if (!gGate.tryEnter()) {
        return original(heap, flags, pointer, size);
    }
    const spark::WindowsHeapReAllocFn target = chooseHandler(gHandlers.heap_realloc_fn, original);
    void *result = target(heap, flags, pointer, size);
    (void)gGate.leave();
    return result;
}

extern "C" __declspec(dllexport) BOOL WINAPI sparkAllocationShimHeapFree(HANDLE heap, DWORD flags,
                                                                          void *pointer) noexcept
{
    const spark::WindowsHeapFreeFn original = gOriginals.heap_free_fn;
    if (original == nullptr) {
        return FALSE;
    }
    if (!gGate.tryEnter()) {
        return original(heap, flags, pointer);
    }
    const spark::WindowsHeapFreeFn target = chooseHandler(gHandlers.heap_free_fn, original);
    const BOOL result = target(heap, flags, pointer);
    (void)gGate.leave();
    return result;
}
