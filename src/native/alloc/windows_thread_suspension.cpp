#include "native/alloc/windows_thread_suspension.h"

#ifndef _WIN32
#error "windows_thread_suspension.cpp must only be compiled on Windows"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows SDK headers require windows.h first
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

namespace spark {
namespace {

constexpr std::size_t KMaxPatchThreads = 2048;

}  // namespace

SuspendedProcessThreads::SuspendedProcessThreads()
{
    threads_.reserve(KMaxPatchThreads);
}

SuspendedProcessThreads::~SuspendedProcessThreads()
{
    std::string ignored;
    resume(ignored);
    closeHandles();
}

// Re-snapshot after each pass to catch threads appearing mid-patch.
// No allocation is permitted after the first thread is suspended.
bool SuspendedProcessThreads::suspendStable(std::string &error)
{
    DWORD failure = ERROR_SUCCESS;
    const char *operation = nullptr;

    for (int pass = 0; pass < 4; ++pass) {
        bool added = false;
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            failure = ::GetLastError();
            operation = "CreateToolhelp32Snapshot";
            break;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (::Thread32First(snapshot, &entry) == FALSE) {
            failure = ::GetLastError();
            operation = "Thread32First";
            ::CloseHandle(snapshot);
            break;
        }

        const DWORD process_id = ::GetCurrentProcessId();
        const DWORD current_thread_id = ::GetCurrentThreadId();
        do {
            if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == current_thread_id ||
                contains(entry.th32ThreadID)) {
                continue;
            }
            if (threads_.size() == threads_.capacity()) {
                failure = ERROR_NOT_ENOUGH_MEMORY;
                operation = "thread suspension capacity";
                break;
            }

            HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                const DWORD code = ::GetLastError();
                if (code == ERROR_INVALID_PARAMETER) {
                    continue;  // thread exited after the snapshot
                }
                failure = code;
                operation = "OpenThread";
                break;
            }

            const DWORD previous_count = ::SuspendThread(thread);
            if (previous_count == std::numeric_limits<DWORD>::max()) {
                DWORD exit_code = STILL_ACTIVE;
                const DWORD code = ::GetLastError();
                if (::GetExitCodeThread(thread, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                    ::CloseHandle(thread);
                    continue;
                }
                ::CloseHandle(thread);
                failure = code;
                operation = "SuspendThread";
                break;
            }

            threads_.push_back({.handle = thread,
                                .thread_id = entry.th32ThreadID,
                                .previous_suspend_count = previous_count,
                                .suspended = true});
            added = true;
        } while (failure == ERROR_SUCCESS && ::Thread32Next(snapshot, &entry) != FALSE);

        if (failure == ERROR_SUCCESS) {
            const DWORD iteration_error = ::GetLastError();
            if (iteration_error != ERROR_NO_MORE_FILES) {
                failure = iteration_error;
                operation = "Thread32Next";
            }
        }

        ::CloseHandle(snapshot);
        if (failure != ERROR_SUCCESS) {
            break;
        }
        if (!added) {
            return true;
        }
    }

    if (failure == ERROR_SUCCESS) {
        failure = ERROR_BUSY;
        operation = "thread set did not stabilize";
    }

    std::string resume_error;
    resume(resume_error);
    error = std::string(operation != nullptr ? operation : "thread suspension") + " failed: " + std::to_string(failure);
    if (!resume_error.empty()) {
        error += "; " + resume_error;
    }
    return false;
}

bool SuspendedProcessThreads::resume(std::string &error) noexcept
{
    DWORD first_failure = ERROR_SUCCESS;
    DWORD first_thread = 0;
    for (auto &thread : std::views::reverse(threads_)) {
        if (!thread.suspended) {
            continue;
        }
        bool resumed = false;
        DWORD last_error = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::ResumeThread(thread.handle) != static_cast<DWORD>(-1)) {
                resumed = true;
                break;
            }
            last_error = ::GetLastError();
            DWORD exit_code = STILL_ACTIVE;
            if (::GetExitCodeThread(thread.handle, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                resumed = true;  // exited threads no longer need resuming
                break;
            }
            ::Sleep(1);
        }
        if (!resumed && first_failure == ERROR_SUCCESS) {
            first_failure = last_error != ERROR_SUCCESS ? last_error : ::GetLastError();
            first_thread = thread.thread_id;
        }
        else if (resumed) {
            thread.suspended = false;
        }
    }

    if (first_failure != ERROR_SUCCESS) {
        try {
            error =
                "ResumeThread failed for thread " + std::to_string(first_thread) + ": " + std::to_string(first_failure);
        }
        catch (...) {
            error.clear();
        }
        // Leaving a thread suspended can deadlock unrelated subsystems; terminate.
        ::TerminateProcess(::GetCurrentProcess(), first_failure);
        std::abort();  // defensive fallback if TerminateProcess unexpectedly returns
    }
    return true;
}

bool SuspendedProcessThreads::anyInstructionPointerInRanges(const std::vector<WindowsCodeRange> &ranges, bool &found,
                                                            std::uint32_t &failure,
                                                            std::uint32_t &failed_thread) const noexcept
{
    found = false;
    failure = ERROR_SUCCESS;
    failed_thread = 0;
    for (const ThreadRecord &thread : threads_) {
        if (!thread.suspended) {
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (::GetThreadContext(thread.handle, &context) == FALSE) {
            DWORD exit_code = STILL_ACTIVE;
            if (::GetExitCodeThread(thread.handle, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                continue;
            }
            failure = static_cast<std::uint32_t>(::GetLastError());
            failed_thread = thread.thread_id;
            return false;
        }
#ifdef _M_X64
        const auto instruction = static_cast<std::uintptr_t>(context.Rip);
#elif defined(_M_ARM64)
        const std::uintptr_t instruction = static_cast<std::uintptr_t>(context.Pc);
#else
#error "Windows allocation profiler requires a supported 64-bit CONTEXT"
#endif
        for (const WindowsCodeRange &range : ranges) {
            if (instruction >= range.begin && instruction < range.end) {
                found = true;
                return true;
            }
        }
    }
    return true;
}

bool SuspendedProcessThreads::contains(std::uint32_t thread_id) const noexcept
{
    return std::ranges::any_of(threads_,
                               [thread_id](const ThreadRecord &record) { return record.thread_id == thread_id; });
}

void SuspendedProcessThreads::closeHandles() noexcept
{
    for (ThreadRecord &thread : threads_) {
        if (thread.handle != nullptr) {
            ::CloseHandle(thread.handle);
            thread.handle = nullptr;
        }
    }
    threads_.clear();
}

}  // namespace spark
