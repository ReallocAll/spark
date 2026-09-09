#ifndef ENDSTONE_SPARK_CAPTURE_WINDOWS_BACKEND_H
#define ENDSTONE_SPARK_CAPTURE_WINDOWS_BACKEND_H

#ifndef _WIN32
#error "capture_windows_backend.h is Windows-only"
#endif

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on

#include "native/sampler/windows_stack_snapshot.h"

namespace spark {

class WindowsCaptureBackend {
public:
    virtual ~WindowsCaptureBackend() = default;

    virtual HANDLE openThread(DWORD thread_id) noexcept = 0;
    virtual DWORD suspendThread(HANDLE thread) noexcept = 0;
    virtual bool getThreadContext(HANDLE thread, CONTEXT &context) noexcept = 0;
    virtual bool captureStackSnapshot(HANDLE thread, const CONTEXT &context,
                                      WindowsStackSnapshot &snapshot) noexcept = 0;
    virtual WindowsWalkStatus unwindNext(const WindowsStackSnapshot &snapshot, CONTEXT &context,
                                         std::uintptr_t &instruction_pointer) noexcept = 0;
    virtual DWORD resumeThread(HANDLE thread) noexcept = 0;
    virtual bool threadExited(HANDLE thread) noexcept = 0;
    virtual void closeThread(HANDLE thread) noexcept = 0;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_CAPTURE_WINDOWS_BACKEND_H
