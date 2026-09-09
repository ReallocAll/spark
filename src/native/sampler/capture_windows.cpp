#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "native/diagnostics/ci_diagnostics.h"
#include "native/sampler/capture.h"
#include "native/sampler/capture_windows_backend.h"
#include "native/symbol/dbghelp_manager.h"

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

    bool captureStackSnapshot(HANDLE, const CONTEXT &context, WindowsStackSnapshot &snapshot) noexcept override
    {
        snapshot.clear();
        const auto stack_pointer = static_cast<std::uintptr_t>(context.Rsp);
        if (stack_pointer == 0 || !windowsCanonicalAddress(stack_pointer)) {
            return false;
        }

        std::uintptr_t cursor = stack_pointer;
        std::size_t copied = 0;
        for (std::size_t query_count = 0;
             query_count < kWindowsStackSnapshotRegionQueryLimit && copied < WindowsStackSnapshot::kMaxBytes;
             ++query_count) {
            MEMORY_BASIC_INFORMATION memory{};
            if (::VirtualQuery(reinterpret_cast<const void *>(cursor), &memory, sizeof(memory)) == 0 ||
                memory.BaseAddress == nullptr || memory.RegionSize == 0 || memory.State != MEM_COMMIT ||
                (memory.Protect & PAGE_GUARD) != 0 || (memory.Protect & 0xffU) == PAGE_NOACCESS ||
                (memory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
                break;
            }
            const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
            if (region_begin > (std::numeric_limits<std::uintptr_t>::max)() - memory.RegionSize) {
                break;
            }
            const auto region_end = region_begin + memory.RegionSize;
            if (cursor < region_begin || cursor >= region_end) {
                break;
            }
            const auto region_remaining = region_end - cursor;
            const auto capacity_remaining = WindowsStackSnapshot::kMaxBytes - copied;
            const auto to_copy = region_remaining < capacity_remaining ? region_remaining : capacity_remaining;
            if (to_copy == 0 || cursor > (std::numeric_limits<std::uintptr_t>::max)() - to_copy) {
                break;
            }
            SIZE_T bytes_read = 0;
            if (::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void *>(cursor),
                                    snapshot.data() + copied, to_copy, &bytes_read) == FALSE ||
                bytes_read == 0) {
                break;
            }
            copied += static_cast<std::size_t>(bytes_read);
            if (bytes_read != to_copy) {
                break;
            }
            cursor += static_cast<std::size_t>(bytes_read);
        }
        return copied != 0 && snapshot.setRange(stack_pointer, copied);
    }

    WindowsWalkStatus unwindNext(const WindowsStackSnapshot &snapshot, CONTEXT &context,
                                 std::uintptr_t &instruction_pointer) noexcept override
    {
        return windowsUnwindNext(snapshot, context, instruction_pointer, lookupFunctionEntry, nullptr, readMemory,
                                 nullptr);
    }

    DWORD resumeThread(HANDLE thread) noexcept override { return ::ResumeThread(thread); }

    bool threadExited(HANDLE thread) noexcept override
    {
        DWORD exit_code = STILL_ACTIVE;
        return ::GetExitCodeThread(thread, &exit_code) != FALSE && exit_code != STILL_ACTIVE;
    }

    void closeThread(HANDLE thread) noexcept override { ::CloseHandle(thread); }

private:
    static WindowsFunctionLookupStatus lookupFunctionEntry(std::uintptr_t control_pc, WindowsRuntimeFunction &function,
                                                           void *) noexcept
    {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION runtime_function =
            ::RtlLookupFunctionEntry(static_cast<DWORD64>(control_pc), &image_base, nullptr);
        if (runtime_function == nullptr) {
            return WindowsFunctionLookupStatus::Leaf;
        }
        if (image_base == 0 || runtime_function->BeginAddress >= runtime_function->EndAddress ||
            static_cast<std::uintptr_t>(runtime_function->BeginAddress) >
                (std::numeric_limits<std::uintptr_t>::max)() - static_cast<std::uintptr_t>(image_base) ||
            static_cast<std::uintptr_t>(runtime_function->EndAddress) >
                (std::numeric_limits<std::uintptr_t>::max)() - static_cast<std::uintptr_t>(image_base) ||
            static_cast<std::uintptr_t>(runtime_function->UnwindData) >
                (std::numeric_limits<std::uintptr_t>::max)() - static_cast<std::uintptr_t>(image_base)) {
            return WindowsFunctionLookupStatus::Failure;
        }
        function.begin = static_cast<std::uintptr_t>(image_base) + runtime_function->BeginAddress;
        function.end = static_cast<std::uintptr_t>(image_base) + runtime_function->EndAddress;
        function.unwind_info = static_cast<std::uintptr_t>(image_base) + runtime_function->UnwindData;
        function.image_base = static_cast<std::uintptr_t>(image_base);
        return WindowsFunctionLookupStatus::Function;
    }

    static bool readMemory(std::uintptr_t address, void *destination, std::size_t bytes, void *) noexcept
    {
        if (address == 0 || destination == nullptr || bytes == 0 ||
            address > (std::numeric_limits<std::uintptr_t>::max)() - bytes) {
            return false;
        }
        SIZE_T bytes_read = 0;
        return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void *>(address), destination, bytes,
                                   &bytes_read) != FALSE &&
               bytes_read == bytes;
    }
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
                    diagnostics_->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureResumed, worker_tid_,
                                          target_tid_, CiDiagnosticCounter::ResumeSuccess, 1);
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
                diagnostics_->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureResumeFailed, worker_tid_,
                                      target_tid_);
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
    WindowsStackSnapshot snapshot{};
    if (diagnostics != nullptr) {
        diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureSuspendAttempt, worker_tid, tid);
    }
    if (backend.suspendThread(thread) == (std::numeric_limits<DWORD>::max)()) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureSuspendFailed, worker_tid,
                                 tid);
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

    const bool snapshot_ok = backend.captureStackSnapshot(thread, context, snapshot);
    suspension_guard.restoreOrFailClosed();
    if (!snapshot_ok || cancelled(cancellation_generation)) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureFailed, worker_tid, tid);
        }
        return false;
    }

    CaptureBuffer captured{};
    std::size_t count = 0;
    if (context.Rip == 0 || !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rip)) || context.Rsp == 0 ||
        !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rsp))) {
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureFailed, worker_tid, tid);
        }
        return false;
    }
    captured.ips[count++] = static_cast<cpptrace::frame_ptr>(context.Rip);

    bool walk_failed = false;
    std::array<std::uintptr_t, kWindowsStackUnwindStepLimit> seen_rips{};
    std::array<std::uintptr_t, kWindowsStackUnwindStepLimit> seen_rsps{};
    std::size_t seen_count = 0;
    if (count != 0) {
        seen_rips[seen_count] = static_cast<std::uintptr_t>(context.Rip);
        seen_rsps[seen_count] = static_cast<std::uintptr_t>(context.Rsp);
        ++seen_count;
    }
    for (std::size_t step = 0; step < kWindowsStackUnwindStepLimit && count < CaptureBuffer::kMax; ++step) {
        std::uintptr_t instruction_pointer = 0;
        if (diagnostics != nullptr) {
            diagnostics->publish(CiDiagnosticContext::Capture, CiDiagnosticPhase::CaptureWalkCall, worker_tid, tid,
                                 CiDiagnosticCounter::WalkCalls, 1);
        }
        const WindowsWalkStatus status = backend.unwindNext(snapshot, context, instruction_pointer);
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
        if (!windowsCanonicalAddress(instruction_pointer) || context.Rsp == 0 ||
            !windowsCanonicalAddress(static_cast<std::uintptr_t>(context.Rsp))) {
            walk_failed = true;
            break;
        }
        bool repeated = false;
        for (std::size_t index = 0; index < seen_count; ++index) {
            if (seen_rips[index] == instruction_pointer &&
                seen_rsps[index] == static_cast<std::uintptr_t>(context.Rsp)) {
                repeated = true;
                break;
            }
        }
        if (repeated || seen_count >= seen_rips.size()) {
            break;
        }
        seen_rips[seen_count] = instruction_pointer;
        seen_rsps[seen_count] = static_cast<std::uintptr_t>(context.Rsp);
        ++seen_count;
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
