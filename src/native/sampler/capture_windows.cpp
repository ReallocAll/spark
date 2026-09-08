#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "native/sampler/capture.h"
#include "native/sampler/capture_windows_backend.h"
#include "native/symbol/dbghelp_manager.h"
#include "core/diagnostics/ci_diagnostics.h"

namespace spark {

namespace {
constexpr std::size_t KMaxThreadCycleEntries = 4096;
constexpr std::size_t KResumeAttempts = 32;
bool GArmed = false;
std::atomic<std::uint64_t> GCancellationGeneration{0};
std::unordered_map<DWORD, ULONG64> GThreadCycles;  // NOLINT(bugprone-throwing-static-initialization)

class SystemWindowsCaptureBackend final : public WindowsCaptureBackend {
public:
    HANDLE openThread(DWORD thread_id) noexcept override
    {
        return ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
    }

    DWORD suspendThread(HANDLE thread) noexcept override { return ::SuspendThread(thread); }

    bool getThreadContext(HANDLE thread, CONTEXT &context) noexcept override
    {
        return ::GetThreadContext(thread, &context) != FALSE;
    }

    bool initializeStackWalk(const CONTEXT &context, STACKFRAME64 &frame) noexcept override
    {
        frame = STACKFRAME64{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        return true;
    }

    WindowsWalkStatus walkNext(HANDLE thread, CONTEXT &context, STACKFRAME64 &frame,
                               std::uintptr_t &instruction_pointer) noexcept override
    {
        if (::StackWalk64(IMAGE_FILE_MACHINE_AMD64, ::GetCurrentProcess(), thread, &frame, &context, nullptr,
                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE ||
            frame.AddrPC.Offset == 0) {
            return WindowsWalkStatus::Complete;
        }
        instruction_pointer = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
        return WindowsWalkStatus::Frame;
    }

    DWORD resumeThread(HANDLE thread) noexcept override { return ::ResumeThread(thread); }

    bool threadExited(HANDLE thread) noexcept override
    {
        DWORD exit_code = STILL_ACTIVE;
        return ::GetExitCodeThread(thread, &exit_code) != FALSE && exit_code != STILL_ACTIVE;
    }

    void closeThread(HANDLE thread) noexcept override { ::CloseHandle(thread); }
};

SystemWindowsCaptureBackend GSystemBackend;
WindowsCaptureBackend *GBackend = &GSystemBackend;

class ThreadHandleGuard {
public:
    ThreadHandleGuard(WindowsCaptureBackend &backend, HANDLE thread) noexcept : backend_(backend), thread_(thread) {}
    ThreadHandleGuard(const ThreadHandleGuard &) = delete;
    ThreadHandleGuard &operator=(const ThreadHandleGuard &) = delete;
    ~ThreadHandleGuard() { backend_.closeThread(thread_); }

private:
    WindowsCaptureBackend &backend_;
    HANDLE thread_;
};

class SuspendedThreadGuard {
public:
    SuspendedThreadGuard(WindowsCaptureBackend &backend, HANDLE thread, CiDiagnostics *diagnostics,
                         std::uint64_t worker_tid, std::uint64_t target_tid) noexcept
        : backend_(backend), thread_(thread), diagnostics_(diagnostics), worker_tid_(worker_tid),
          target_tid_(target_tid)
    {
    }
    SuspendedThreadGuard(const SuspendedThreadGuard &) = delete;
    SuspendedThreadGuard &operator=(const SuspendedThreadGuard &) = delete;
    ~SuspendedThreadGuard() { restoreOrFailClosed(); }

    void restoreOrFailClosed() noexcept
    {
        if (!suspended_) {
            return;
        }
        for (std::size_t attempt = 0; attempt < KResumeAttempts; ++attempt) {
            if (diagnostics_ != nullptr) {
                diagnostics_->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureResumeAttempt,
                                      worker_tid_, target_tid_);
            }
            if (backend_.resumeThread(thread_) != (std::numeric_limits<DWORD>::max)()) {
                suspended_ = false;
                if (diagnostics_ != nullptr) {
                    diagnostics_->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureResumed,
                                          worker_tid_, target_tid_, CiDiagnosticCounter::ResumeSuccess, 1);
                }
                return;
            }
            if (backend_.threadExited(thread_)) {
                suspended_ = false;
                if (diagnostics_ != nullptr) {
                    diagnostics_->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureTargetExited,
                                          worker_tid_, target_tid_);
                }
                return;
            }
            if (diagnostics_ != nullptr) {
                diagnostics_->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureResumeFailed,
                                      worker_tid_, target_tid_);
            }
            ::SwitchToThread();
        }
        if (diagnostics_ != nullptr) {
            diagnostics_->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureResumeFailed, worker_tid_,
                                  target_tid_);
        }
        const DWORD last_error = ::GetLastError();
        const DWORD exit_code = last_error != ERROR_SUCCESS ? last_error : ERROR_GEN_FAILURE;
        ::TerminateProcess(::GetCurrentProcess(), exit_code);
        std::abort();
    }

private:
    WindowsCaptureBackend &backend_;
    HANDLE thread_;
    CiDiagnostics *diagnostics_;
    std::uint64_t worker_tid_;
    std::uint64_t target_tid_;
    bool suspended_ = true;
};

bool cancelled(std::uint64_t generation) noexcept
{
    return GCancellationGeneration.load(std::memory_order_acquire) != generation;
}

}  // namespace

bool Capture::arm()
{
    {
        std::scoped_lock lock(dbgHelpMutex());
        if (GArmed) {
            return true;
        }
    }
    if (!retainDbgHelp()) {
        return false;
    }
    std::scoped_lock lock(dbgHelpMutex());
    GThreadCycles.clear();
    GArmed = true;
    if (CiDiagnostics *diagnostics = globalCiDiagnostics(); diagnostics != nullptr) {
        diagnostics->publish(samplerLifecycleDiagnosticContext(), CiDiagnosticPhase::SamplerDbgHelpAcquired,
                             ciDiagnosticCurrentThreadId());
    }
    return true;
}

bool Capture::disarm()
{
    cancelPending();
    {
        std::scoped_lock lock(dbgHelpMutex());
        if (!GArmed) {
            return true;
        }
        GThreadCycles.clear();
        GArmed = false;
    }
    releaseDbgHelp();
    return true;
}

void Capture::cancelPending() noexcept
{
    GCancellationGeneration.fetch_add(1, std::memory_order_acq_rel);
}

bool Capture::captureThread(std::uint64_t tid, CaptureBuffer &out)
{
    out.count = 0;
    const std::uint64_t cancellation_generation = GCancellationGeneration.load(std::memory_order_acquire);
    CiDiagnostics *diagnostics = globalCiDiagnostics();
    const std::uint64_t worker_tid = ::GetCurrentThreadId();
    if (diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureEnter, worker_tid, tid);
    }
    std::scoped_lock lock(dbgHelpMutex());
    if (!GArmed || cancelled(cancellation_generation) || static_cast<DWORD>(tid) == ::GetCurrentThreadId()) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureFailed, worker_tid, tid);
        }
        return false;
    }

    WindowsCaptureBackend &backend = *GBackend;
    HANDLE thread = backend.openThread(static_cast<DWORD>(tid));
    if (thread == nullptr) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureOpenFailed, worker_tid, tid);
        }
        return false;
    }
    ThreadHandleGuard handle_guard(backend, thread);
    if (diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureSuspendAttempt, worker_tid, tid);
    }
    if (backend.suspendThread(thread) == (std::numeric_limits<DWORD>::max)()) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureSuspendFailed, worker_tid, tid);
        }
        return false;
    }
    SuspendedThreadGuard suspension_guard(backend, thread, diagnostics, worker_tid, tid);
    if (diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureSuspended, worker_tid, tid,
                             CiDiagnosticCounter::SuspendSuccess, 1);
    }
    if (cancelled(cancellation_generation)) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureFailed, worker_tid, tid);
        }
        return false;
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureContextCall, worker_tid, tid);
    }
    const bool context_ok = backend.getThreadContext(thread, context);
    if (diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureContextReturn, worker_tid, tid);
    }
    if (!context_ok || cancelled(cancellation_generation)) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureFailed, worker_tid, tid);
        }
        return false;
    }

    STACKFRAME64 frame{};
    if (!backend.initializeStackWalk(context, frame) || cancelled(cancellation_generation)) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureFailed, worker_tid, tid);
        }
        return false;
    }

    CaptureBuffer captured{};
    std::size_t count = 0;
    if (context.Rip != 0) {
        captured.ips[count++] = static_cast<cpptrace::frame_ptr>(context.Rip);
    }

    bool walk_failed = false;
    while (count < CaptureBuffer::kMax) {
        std::uintptr_t instruction_pointer = 0;
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureWalkCall, worker_tid, tid,
                                 CiDiagnosticCounter::WalkCalls, 1);
        }
        const WindowsWalkStatus status = backend.walkNext(thread, context, frame, instruction_pointer);
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureWalkReturn, worker_tid, tid);
        }
        if (status == WindowsWalkStatus::Failure) {
            walk_failed = true;
            break;
        }
        if (status == WindowsWalkStatus::Complete || instruction_pointer == 0) {
            break;
        }
        const auto ip = static_cast<cpptrace::frame_ptr>(instruction_pointer);
        if (count == 0 || ip != captured.ips[count - 1]) {
            captured.ips[count++] = ip;
        }
        if (cancelled(cancellation_generation)) {
            walk_failed = true;
            break;
        }
    }
    captured.count = count;

    suspension_guard.restoreOrFailClosed();
    if (walk_failed || cancelled(cancellation_generation) || captured.count == 0) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureFailed, worker_tid, tid);
        }
        return false;
    }
    out = captured;
    if (diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureComplete, worker_tid, tid);
    }
    return true;
}

bool Capture::isThreadRunning(std::uint64_t tid)
{
    std::scoped_lock lock(dbgHelpMutex());
    if (!GArmed) {
        return true;
    }

    const auto thread_id = static_cast<DWORD>(tid);
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
    if (thread == nullptr) {
        return true;
    }
    ULONG64 cycles = 0;
    const bool queried = QueryThreadCycleTime(thread, &cycles) != FALSE;
    CloseHandle(thread);
    if (!queried) {
        return true;
    }

    auto it = GThreadCycles.find(thread_id);
    if (it == GThreadCycles.end()) {
        if (GThreadCycles.size() == KMaxThreadCycleEntries) {
            GThreadCycles.erase(GThreadCycles.begin());
        }
        GThreadCycles.emplace(thread_id, cycles);
        return false;
    }
    const ULONG64 previous = it->second;
    it->second = cycles;
    return cycles != previous;
}

void Capture::setWindowsBackendForTesting(WindowsCaptureBackend *backend)
{
    std::scoped_lock lock(dbgHelpMutex());
    if (GArmed) {
        std::abort();
    }
    GBackend = backend != nullptr ? backend : &GSystemBackend;
}

std::uint64_t Capture::cancellationGenerationForTesting() noexcept
{
    return GCancellationGeneration.load(std::memory_order_acquire);
}

}  // namespace spark
