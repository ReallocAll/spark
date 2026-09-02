#include "native/alloc/windows_stable_entry_atomic.h"

#ifndef _WIN32
#error "windows_permanent_gateway_guarded_publish.cpp is Windows-only"
#endif

#include <array>
#include <cstdint>
#include <cstring>

namespace spark::stable_entry_experiment {
namespace {

constexpr std::size_t kMaxPublishThreads = 4096;
constexpr std::uint64_t kPublishTimeoutMs = 5000;

struct SuspendedThread {
    HANDLE handle = nullptr;
    DWORD id = 0;
};

[[noreturn]] void terminateUnsafePublish(DWORD code) noexcept
{
    ::TerminateProcess(::GetCurrentProcess(), code != ERROR_SUCCESS ? code : ERROR_GEN_FAILURE);
    std::abort();
}

void resumeAll(std::array<SuspendedThread, kMaxPublishThreads> &threads, std::size_t count) noexcept
{
    for (std::size_t index = count; index != 0; --index) {
        SuspendedThread &thread = threads[index - 1];
        if (thread.handle == nullptr) {
            continue;
        }
        if (::ResumeThread(thread.handle) == static_cast<DWORD>(-1)) {
            terminateUnsafePublish(::GetLastError());
        }
        ::CloseHandle(thread.handle);
        thread.handle = nullptr;
    }
}

[[nodiscard]] bool containsThread(const std::array<SuspendedThread, kMaxPublishThreads> &threads, std::size_t count,
                                  DWORD id) noexcept
{
    for (std::size_t index = 0; index < count; ++index) {
        if (threads[index].id == id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool suspendToFixedPoint(std::array<SuspendedThread, kMaxPublishThreads> &threads, std::size_t &count,
                                       DWORD current_thread) noexcept
{
    const DWORD process_id = ::GetCurrentProcessId();
    for (;;) {
        bool added = false;
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        BOOL more = ::Thread32First(snapshot, &entry);
        while (more != FALSE) {
            if (entry.th32OwnerProcessID == process_id && entry.th32ThreadID != current_thread &&
                !containsThread(threads, count, entry.th32ThreadID)) {
                if (count == threads.size()) {
                    ::CloseHandle(snapshot);
                    return false;
                }
                HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                             FALSE, entry.th32ThreadID);
                if (thread != nullptr) {
                    const DWORD previous = ::SuspendThread(thread);
                    if (previous == static_cast<DWORD>(-1)) {
                        const DWORD failure = ::GetLastError();
                        DWORD exit_code = STILL_ACTIVE;
                        const bool exited = ::GetExitCodeThread(thread, &exit_code) != FALSE && exit_code != STILL_ACTIVE;
                        ::CloseHandle(thread);
                        if (!exited && failure != ERROR_INVALID_PARAMETER) {
                            ::CloseHandle(snapshot);
                            return false;
                        }
                    }
                    else {
                        threads[count++] = {.handle = thread, .id = entry.th32ThreadID};
                        added = true;
                    }
                }
                else {
                    const DWORD failure = ::GetLastError();
                    if (failure != ERROR_INVALID_PARAMETER) {
                        ::CloseHandle(snapshot);
                        return false;
                    }
                }
            }
            more = ::Thread32Next(snapshot, &entry);
        }
        ::CloseHandle(snapshot);

        // Once an entire enumeration adds no thread, every in-process creator
        // observed by the previous pass is already suspended. A thread born
        // just before its creator was suspended is discovered by this fixed-point
        // pass before publication proceeds.
        if (!added) {
            return true;
        }
    }
}

[[nodiscard]] bool anyRipInPatchWindow(const std::array<SuspendedThread, kMaxPublishThreads> &threads,
                                       std::size_t count, std::uintptr_t begin, std::uintptr_t end) noexcept
{
    for (std::size_t index = 0; index < count; ++index) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (::GetThreadContext(threads[index].handle, &context) == FALSE) {
            return true;
        }
#if defined(_M_X64) || defined(__x86_64__)
        const std::uintptr_t rip = static_cast<std::uintptr_t>(context.Rip);
#else
#error "permanent gateway guarded publication requires Windows x64"
#endif
        if (begin <= rip && rip < end) {
            return true;
        }
    }
    return false;
}

AtomicCompareResult failedObservation(void *address) noexcept
{
    AtomicCompareResult result;
    if (address != nullptr) {
        std::memcpy(result.observed.data(), address, sizeof(std::uint64_t));
    }
    result.exchanged = false;
    return result;
}

}  // namespace

// Experimental guarded replacement for the one permanent executable publication.
// The permanent-gateway translation unit is compiled with its atomic8 call mapped
// to this symbol. All relocation/allocation work therefore completes before any
// peer thread is suspended; only the final executable-byte transaction is inside
// the fixed-point stop-the-world window.
AtomicCompareResult atomicCompareExchange8Quiesced(void *address, const std::array<std::uint8_t, 16> &expected,
                                                    const std::array<std::uint8_t, 16> &desired) noexcept
{
    if (address == nullptr || !isAlignedForAtomic8(reinterpret_cast<std::uintptr_t>(address))) {
        return failedObservation(address);
    }

    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t end = begin + kAtomicEntryWidth8;
    const std::uint64_t deadline = ::GetTickCount64() + kPublishTimeoutMs;

    for (;;) {
        std::array<SuspendedThread, kMaxPublishThreads> threads{};
        std::size_t count = 0;
        const DWORD current_thread = ::GetCurrentThreadId();
        if (!suspendToFixedPoint(threads, count, current_thread)) {
            resumeAll(threads, count);
            return failedObservation(address);
        }

        if (anyRipInPatchWindow(threads, count, begin, end)) {
            resumeAll(threads, count);
            if (::GetTickCount64() >= deadline) {
                return failedObservation(address);
            }
            ::SwitchToThread();
            continue;
        }

        AtomicCompareResult result = atomicCompareExchange8(address, expected, desired);
        if (result.exchanged &&
            ::FlushInstructionCache(::GetCurrentProcess(), address, kAtomicEntryWidth8) == FALSE) {
            const DWORD failure = ::GetLastError();
            // Returning after publishing executable bytes without establishing
            // the instruction-cache visibility boundary would let the caller
            // reclaim still-referenced preparation state. Terminate instead.
            terminateUnsafePublish(failure);
        }
        resumeAll(threads, count);
        return result;
    }
}

}  // namespace spark::stable_entry_experiment
