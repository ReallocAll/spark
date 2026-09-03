#pragma once

#ifndef _WIN32
#error "windows_thread_suspension.h must only be included on Windows"
#endif

#include <cstdint>
#include <string>
#include <vector>

#if defined(ENDSTONE_SPARK_WINDOWS_PERMANENT_IAT_EXPERIMENT)
#include "native/alloc/windows_dynamic_stack_capture.h"
// allocation_sampler_windows.cpp includes this header after the Windows SDK.
// Keep the production shim backend on the native fast walker, while the
// permanent-IAT experiment substitutes a walker that honors dynamic unwind
// tables. The leading global scope in ::RtlCaptureStackBackTrace expands to a
// valid ::spark::captureDynamicAwareStackBackTrace call.
#define RtlCaptureStackBackTrace spark::captureDynamicAwareStackBackTrace
#endif

namespace spark {

struct WindowsCodeRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

class SuspendedProcessThreads {
public:
    SuspendedProcessThreads();

    SuspendedProcessThreads(const SuspendedProcessThreads &) = delete;
    SuspendedProcessThreads &operator=(const SuspendedProcessThreads &) = delete;

    ~SuspendedProcessThreads();

    bool suspendStable(std::string &error);
    bool resume(std::string &error) noexcept;
    bool anyInstructionPointerInRanges(const std::vector<WindowsCodeRange> &ranges, bool &found, std::uint32_t &failure,
                                       std::uint32_t &failed_thread) const noexcept;

private:
    struct ThreadRecord {
        void *handle = nullptr;
        std::uint32_t thread_id = 0;
        std::uint32_t previous_suspend_count = 0;
        bool suspended = false;
    };

    [[nodiscard]] bool contains(std::uint32_t thread_id) const noexcept;
    void closeHandles() noexcept;

    std::vector<ThreadRecord> threads_;
};

}  // namespace spark
