#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" __declspec(dllimport) void *sparkIatProviderAlloc(std::size_t size);

namespace {

std::atomic<HANDLE> gEnteredEvent{nullptr};
std::atomic<HANDLE> gReleaseEvent{nullptr};
std::atomic<std::uint64_t> gCalls{0};

}  // namespace

extern "C" __declspec(dllexport) void sparkIatHandlerSetBlockEvents(HANDLE entered, HANDLE release) noexcept
{
    gEnteredEvent.store(entered, std::memory_order_release);
    gReleaseEvent.store(release, std::memory_order_release);
}

extern "C" __declspec(dllexport) void *__cdecl sparkIatHandlerAlloc(std::size_t size) noexcept
{
    HANDLE entered = gEnteredEvent.load(std::memory_order_acquire);
    HANDLE release = gReleaseEvent.load(std::memory_order_acquire);
    if (entered != nullptr && release != nullptr) {
        (void)::SetEvent(entered);
        (void)::WaitForSingleObject(release, INFINITE);
    }
    gCalls.fetch_add(1, std::memory_order_relaxed);
    return sparkIatProviderAlloc(size);
}

extern "C" __declspec(dllexport) std::uint64_t sparkIatHandlerCalls() noexcept
{
    return gCalls.load(std::memory_order_relaxed);
}
