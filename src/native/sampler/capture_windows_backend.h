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
#include <dbghelp.h>
// clang-format on

namespace spark {

enum class WindowsWalkStatus {
    Frame,
    Complete,
    Failure,
};

class WindowsCaptureBackend {
public:
    virtual ~WindowsCaptureBackend() = default;

    virtual HANDLE openThread(DWORD thread_id) noexcept = 0;
    virtual DWORD suspendThread(HANDLE thread) noexcept = 0;
    virtual bool getThreadContext(HANDLE thread, CONTEXT &context) noexcept = 0;
    virtual bool initializeStackWalk(const CONTEXT &context, STACKFRAME64 &frame) noexcept = 0;
    virtual WindowsWalkStatus walkNext(HANDLE thread, CONTEXT &context, STACKFRAME64 &frame,
                                       std::uintptr_t &instruction_pointer) noexcept = 0;
    virtual DWORD resumeThread(HANDLE thread) noexcept = 0;
    virtual bool threadExited(HANDLE thread) noexcept = 0;
    virtual void closeThread(HANDLE thread) noexcept = 0;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_CAPTURE_WINDOWS_BACKEND_H
