#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>

#include "native/sampler/capture.h"
#include "native/sampler/capture_windows_backend.h"
#include "native/sampler/sampler.h"

namespace spark {

struct CaptureTestAccess {
    static void setWindowsBackend(WindowsCaptureBackend *backend) { Capture::setWindowsBackendForTesting(backend); }
    static std::uint64_t cancellationGeneration() { return Capture::cancellationGenerationForTesting(); }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;
constexpr std::uint64_t KFakeThreadId = 0x5A17;

class FakeWindowsCaptureBackend final : public spark::WindowsCaptureBackend {
public:
    void configureSuccess()
    {
        std::scoped_lock lock(mutex_);
        open_ok_ = true;
        suspend_ok_ = true;
        context_ok_ = true;
        initialize_ok_ = true;
        walk_failure_at_ = -1;
        cancel_on_suspend_ = false;
        resume_failures_remaining_ = 0;
        block_context_ = false;
        context_entered_ = false;
        release_context_ = false;
        walk_index_ = 0;
        successful_suspends_ = 0;
        resume_calls_ = 0;
        successful_resumes_ = 0;
        close_calls_ = 0;
        context_calls_ = 0;
        initialize_calls_ = 0;
        walk_calls_ = 0;
        closed_while_suspended_ = false;
        resume_without_suspend_ = false;
    }

    void setInitialSuspendCount(DWORD count)
    {
        std::scoped_lock lock(mutex_);
        baseline_suspend_count_ = count;
        suspend_count_ = count;
    }

    void failOpen() { setFlag(open_ok_, false); }
    void failSuspend() { setFlag(suspend_ok_, false); }
    void failContext() { setFlag(context_ok_, false); }
    void failInitialize() { setFlag(initialize_ok_, false); }
    void failWalkAt(int index)
    {
        std::scoped_lock lock(mutex_);
        walk_failure_at_ = index;
    }
    void cancelOnSuspend() { setFlag(cancel_on_suspend_, true); }

    void failResumeTimes(int count)
    {
        std::scoped_lock lock(mutex_);
        resume_failures_remaining_ = count;
    }

    void blockContext()
    {
        std::scoped_lock lock(mutex_);
        block_context_ = true;
        context_entered_ = false;
        release_context_ = false;
    }

    bool waitForContext(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return context_entered_; });
    }

    void releaseContext()
    {
        {
            std::scoped_lock lock(mutex_);
            release_context_ = true;
        }
        condition_.notify_all();
    }

    bool waitForSuccessfulResume(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, count] { return successful_resumes_ >= count; });
    }

    HANDLE openThread(DWORD) noexcept override
    {
        std::scoped_lock lock(mutex_);
        return open_ok_ ? static_cast<HANDLE>(&fake_thread_) : nullptr;
    }

    DWORD suspendThread(HANDLE) noexcept override
    {
        bool cancel = false;
        DWORD previous = (std::numeric_limits<DWORD>::max)();
        {
            std::scoped_lock lock(mutex_);
            if (suspend_ok_) {
                previous = suspend_count_;
                ++suspend_count_;
                ++successful_suspends_;
                cancel = cancel_on_suspend_;
            }
        }
        if (cancel) {
            spark::Capture::cancelPending();
        }
        return previous;
    }

    bool getThreadContext(HANDLE, CONTEXT &context) noexcept override
    {
        std::unique_lock lock(mutex_);
        ++context_calls_;
        context_entered_ = true;
        condition_.notify_all();
        if (block_context_) {
            condition_.wait(lock, [this] { return release_context_; });
        }
        if (!context_ok_) {
            return false;
        }
        context.Rip = 0x1000;
        context.Rbp = 0x2000;
        context.Rsp = 0x3000;
        return true;
    }

    bool initializeStackWalk(const CONTEXT &, STACKFRAME64 &) noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++initialize_calls_;
        return initialize_ok_;
    }

    spark::WindowsWalkStatus walkNext(HANDLE, CONTEXT &, STACKFRAME64 &,
                                      std::uintptr_t &instruction_pointer) noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++walk_calls_;
        const int index = walk_index_++;
        if (walk_failure_at_ == index) {
            return spark::WindowsWalkStatus::Failure;
        }
        if (index == 0) {
            instruction_pointer = 0x2000;
            return spark::WindowsWalkStatus::Frame;
        }
        return spark::WindowsWalkStatus::Complete;
    }

    DWORD resumeThread(HANDLE) noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++resume_calls_;
        if (resume_failures_remaining_ > 0) {
            --resume_failures_remaining_;
            return (std::numeric_limits<DWORD>::max)();
        }
        if (suspend_count_ <= baseline_suspend_count_) {
            resume_without_suspend_ = true;
            return (std::numeric_limits<DWORD>::max)();
        }
        const DWORD previous = suspend_count_;
        --suspend_count_;
        ++successful_resumes_;
        condition_.notify_all();
        return previous;
    }

    bool threadExited(HANDLE) noexcept override { return false; }

    void closeThread(HANDLE) noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++close_calls_;
        if (suspend_count_ > baseline_suspend_count_) {
            closed_while_suspended_ = true;
        }
    }

    std::size_t successfulSuspends() const
    {
        std::scoped_lock lock(mutex_);
        return successful_suspends_;
    }
    std::size_t resumeCalls() const
    {
        std::scoped_lock lock(mutex_);
        return resume_calls_;
    }
    std::size_t successfulResumes() const
    {
        std::scoped_lock lock(mutex_);
        return successful_resumes_;
    }
    std::size_t closeCalls() const
    {
        std::scoped_lock lock(mutex_);
        return close_calls_;
    }
    std::size_t contextCalls() const
    {
        std::scoped_lock lock(mutex_);
        return context_calls_;
    }
    std::size_t initializeCalls() const
    {
        std::scoped_lock lock(mutex_);
        return initialize_calls_;
    }
    std::size_t walkCalls() const
    {
        std::scoped_lock lock(mutex_);
        return walk_calls_;
    }

    bool balanced() const
    {
        std::scoped_lock lock(mutex_);
        return suspend_count_ == baseline_suspend_count_ && successful_suspends_ == successful_resumes_ &&
               !closed_while_suspended_ && !resume_without_suspend_;
    }

private:
    void setFlag(bool &flag, bool value)
    {
        std::scoped_lock lock(mutex_);
        flag = value;
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ok_ = true;
    bool suspend_ok_ = true;
    bool context_ok_ = true;
    bool initialize_ok_ = true;
    int walk_failure_at_ = -1;
    bool cancel_on_suspend_ = false;
    bool block_context_ = false;
    bool context_entered_ = false;
    bool release_context_ = false;
    int resume_failures_remaining_ = 0;
    int walk_index_ = 0;
    DWORD baseline_suspend_count_ = 0;
    DWORD suspend_count_ = 0;
    int fake_thread_ = 0;
    std::size_t successful_suspends_ = 0;
    std::size_t resume_calls_ = 0;
    std::size_t successful_resumes_ = 0;
    std::size_t close_calls_ = 0;
    std::size_t context_calls_ = 0;
    std::size_t initialize_calls_ = 0;
    std::size_t walk_calls_ = 0;
    bool closed_while_suspended_ = false;
    bool resume_without_suspend_ = false;
};

class CaptureSession {
public:
    explicit CaptureSession(FakeWindowsCaptureBackend &backend) : backend_(backend)
    {
        spark::CaptureTestAccess::setWindowsBackend(&backend_);
        armed_ = spark::Capture::arm();
    }

    ~CaptureSession()
    {
        if (armed_) {
            spark::Capture::disarm();
        }
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
    }

    [[nodiscard]] bool armed() const { return armed_; }

private:
    FakeWindowsCaptureBackend &backend_;
    bool armed_ = false;
};

bool require(bool condition, const char *message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "windows capture lifecycle: %s\n", message);
    return false;
}

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

bool testFailureStages()
{
    FakeWindowsCaptureBackend backend;
    CaptureSession session(backend);
    if (!require(session.armed(), "arm failed")) {
        return false;
    }

    spark::CaptureBuffer buffer{};

    backend.configureSuccess();
    backend.failOpen();
    buffer.count = 99;
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "OpenThread failure did not fail cleanly") ||
        !require(backend.successfulSuspends() == 0 && backend.resumeCalls() == 0 && backend.closeCalls() == 0,
                 "OpenThread failure touched suspension state")) {
        return false;
    }

    backend.configureSuccess();
    backend.failSuspend();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "SuspendThread failure did not fail cleanly") ||
        !require(backend.resumeCalls() == 0 && backend.closeCalls() == 1 && backend.balanced(),
                 "SuspendThread failure resumed an unsuspended thread")) {
        return false;
    }

    backend.configureSuccess();
    backend.cancelOnSuspend();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer), "post-suspend cancellation was ignored") ||
        !require(backend.contextCalls() == 0 && backend.successfulResumes() == 1 && backend.balanced(),
                 "post-suspend cancellation did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    backend.failContext();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "GetThreadContext failure leaked a partial capture") ||
        !require(backend.contextCalls() == 1 && backend.initializeCalls() == 0 && backend.successfulResumes() == 1 &&
                     backend.balanced(),
                 "GetThreadContext failure did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    backend.failInitialize();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "stack initialization failure leaked a partial capture") ||
        !require(backend.initializeCalls() == 1 && backend.walkCalls() == 0 && backend.successfulResumes() == 1 &&
                     backend.balanced(),
                 "stack initialization failure did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    backend.failWalkAt(0);
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "first stack-walk failure published a partial capture") ||
        !require(backend.walkCalls() == 1 && backend.successfulResumes() == 1 && backend.balanced(),
                 "first stack-walk failure did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    backend.failWalkAt(1);
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "mid stack-walk failure published a partial capture") ||
        !require(backend.walkCalls() == 2 && backend.successfulResumes() == 1 && backend.balanced(),
                 "mid stack-walk failure did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    if (!require(spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 2,
                 "successful capture did not publish expected frames") ||
        !require(backend.successfulResumes() == 1 && backend.balanced(), "successful capture did not restore target")) {
        return false;
    }

    backend.configureSuccess();
    backend.failContext();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "failed capture retained frames from the previous capture") ||
        !require(backend.balanced(), "failed reuse left the target suspended")) {
        return false;
    }

    backend.configureSuccess();
    return require(spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 2,
                   "capture backend did not recover after a failure") &&
           require(backend.balanced(), "recovered capture left suspension state changed");
}

bool testResumeRetryAndPriorSuspendCount()
{
    FakeWindowsCaptureBackend backend;
    backend.setInitialSuspendCount(2);
    CaptureSession session(backend);
    if (!require(session.armed(), "arm failed for resume retry")) {
        return false;
    }

    backend.configureSuccess();
    backend.failResumeTimes(1);
    spark::CaptureBuffer buffer{};
    return require(spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 2,
                   "transient ResumeThread failure prevented capture recovery") &&
           require(backend.resumeCalls() == 2 && backend.successfulResumes() == 1,
                   "ResumeThread retry produced the wrong successful resume count") &&
           require(backend.balanced(), "pre-existing suspend count was not restored");
}

bool testDisarmCancellationAndRearm()
{
    FakeWindowsCaptureBackend backend;
    spark::CaptureTestAccess::setWindowsBackend(&backend);
    if (!require(spark::Capture::arm(), "arm failed for disarm cancellation")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    backend.configureSuccess();
    backend.blockContext();
    spark::CaptureBuffer buffer{};
    std::atomic<bool> capture_result{true};
    std::thread capture(
        [&] { capture_result.store(spark::Capture::captureThread(KFakeThreadId, buffer), std::memory_order_release); });
    if (!require(backend.waitForContext(2s), "capture did not reach the blocked context stage")) {
        backend.releaseContext();
        capture.join();
        spark::Capture::disarm();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    const std::uint64_t generation = spark::CaptureTestAccess::cancellationGeneration();
    std::atomic<bool> disarm_result{false};
    std::thread disarm([&] { disarm_result.store(spark::Capture::disarm(), std::memory_order_release); });
    if (!require(waitFor([&] { return spark::CaptureTestAccess::cancellationGeneration() != generation; }, 2s),
                 "disarm did not cancel the in-flight capture")) {
        backend.releaseContext();
        capture.join();
        disarm.join();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    backend.releaseContext();
    capture.join();
    disarm.join();

    if (!require(!capture_result.load(std::memory_order_acquire) && disarm_result.load(std::memory_order_acquire),
                 "disarm cancellation did not complete cleanly") ||
        !require(backend.successfulResumes() == 1 && backend.closeCalls() == 1 && backend.balanced(),
                 "disarm closed the target before restoring it")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    backend.configureSuccess();
    if (!require(spark::Capture::arm(), "rearm failed after cancelled capture")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    const bool restarted = spark::Capture::captureThread(KFakeThreadId, buffer);
    const bool final_disarm = spark::Capture::disarm();
    spark::CaptureTestAccess::setWindowsBackend(nullptr);
    return require(restarted && buffer.count == 2, "capture did not recover after disarm/rearm") &&
           require(final_disarm && backend.balanced(), "final disarm did not restore lifecycle state");
}

bool testSamplerStopAndRestart()
{
    FakeWindowsCaptureBackend backend;
    spark::CaptureTestAccess::setWindowsBackend(&backend);
    backend.configureSuccess();
    backend.blockContext();

    spark::Sampler sampler;
    sampler.setTarget(KFakeThreadId, "Fake server thread");
    spark::SamplerConfig config;
    config.interval_us = 1000;
    if (!require(sampler.start(config), "sampler start failed")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    if (!require(backend.waitForContext(2s), "sampler capture did not reach blocked context")) {
        backend.releaseContext();
        sampler.stop();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    const std::uint64_t generation = spark::CaptureTestAccess::cancellationGeneration();
    std::atomic<bool> stop_result{false};
    std::thread stopper([&] { stop_result.store(sampler.stop(), std::memory_order_release); });
    if (!require(waitFor([&] { return spark::CaptureTestAccess::cancellationGeneration() != generation; }, 2s),
                 "sampler stop did not cancel the in-flight capture")) {
        backend.releaseContext();
        stopper.join();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    backend.releaseContext();
    stopper.join();

    if (!require(stop_result.load(std::memory_order_acquire) && backend.balanced(),
                 "sampler stop did not restore the suspended target")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    backend.configureSuccess();
    if (!require(sampler.start(config), "sampler did not restart after cancelled capture")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    const bool captured_again = backend.waitForSuccessfulResume(1, 2s);
    const bool stopped_again = sampler.stop();
    spark::CaptureTestAccess::setWindowsBackend(nullptr);
    return require(captured_again && backend.walkCalls() >= 2, "sampler did not capture again after restart") &&
           require(stopped_again && backend.balanced(), "restarted sampler did not shut down cleanly");
}

}  // namespace

int main()
{
    if (!testFailureStages() || !testResumeRetryAndPriorSuspendCount() || !testDisarmCancellationAndRearm() ||
        !testSamplerStopAndRestart()) {
        return 1;
    }
    return 0;
}
