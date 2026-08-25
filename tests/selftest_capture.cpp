#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "selftest_internal.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "native/sampler/capture.h"

namespace spark {

struct CaptureTestAccess {
#ifdef __linux__
    static void setHandlerGate(std::atomic<bool> *entered, std::atomic<bool> *release)
    {
        Capture::setHandlerGateForTesting(entered, release);
    }
#endif
};

}  // namespace spark

namespace spark::selftest {

bool verifyCaptureLifecycle()
{
    for (int i = 0; i < 3; ++i) {
        if (!spark::Capture::arm()) {
            std::fprintf(stderr, "capture lifecycle: arm failed on iteration %d\n", i + 1);
            return false;
        }
        spark::Capture::disarm();
    }
    return true;
}

#ifdef __linux__
bool verifyActiveCaptureTeardown(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    if (!spark::Capture::arm()) {
        return false;
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    spark::CaptureTestAccess::setHandlerGate(&entered, &release);
    spark::CaptureBuffer buffer;
    std::thread capture([&] { spark::Capture::captureThread(worker_tid, buffer); });
    if (!waitForCondition([&] { return entered.load(std::memory_order_acquire); }, 2s)) {
        release.store(true, std::memory_order_release);
        capture.join();
        spark::CaptureTestAccess::setHandlerGate(nullptr, nullptr);
        spark::Capture::disarm();
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    const bool unsafe_success = spark::Capture::disarm();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    release.store(true, std::memory_order_release);
    capture.join();
    spark::CaptureTestAccess::setHandlerGate(nullptr, nullptr);
    if (unsafe_success || elapsed > 2s || !spark::Capture::disarm()) {
        std::fprintf(stderr, "active capture teardown: handler state was not retained safely\n");
        return false;
    }
    if (!spark::Capture::arm() || !spark::Capture::disarm()) {
        std::fprintf(stderr, "active capture teardown: capture backend did not restart\n");
        return false;
    }
    return true;
}

bool verifyDelayedSignalLifecycle()
{
    using namespace std::chrono_literals;

    auto start_blocked_target = [](std::atomic<std::uint64_t> &tid, std::atomic<bool> &unblock,
                                   std::atomic<bool> &unblocked, std::atomic<bool> &run) {
        return std::thread([&tid, &unblock, &unblocked, &run] {
            sigset_t signals;
            sigemptyset(&signals);
            sigaddset(&signals, SIGPROF);
            pthread_sigmask(SIG_BLOCK, &signals, nullptr);
            tid.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)), std::memory_order_release);
            while (!unblock.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
            unblocked.store(true, std::memory_order_release);
            while (run.load(std::memory_order_acquire)) {
                hotOuter();
            }
        });
    };

    std::atomic<std::uint64_t> tid{0};
    std::atomic<bool> unblock{false};
    std::atomic<bool> unblocked{false};
    std::atomic<bool> run{true};
    std::thread target = start_blocked_target(tid, unblock, unblocked, run);
    if (!waitForCondition([&] { return tid.load(std::memory_order_acquire) != 0; }, 1s) || !spark::Capture::arm()) {
        unblock.store(true, std::memory_order_release);
        run.store(false, std::memory_order_release);
        target.join();
        return false;
    }
    spark::CaptureBuffer first;
    if (spark::Capture::captureThread(tid.load(std::memory_order_acquire), first)) {
        std::fprintf(stderr, "delayed signal: blocked delivery unexpectedly completed\n");
        spark::Capture::disarm();
        unblock.store(true, std::memory_order_release);
        run.store(false, std::memory_order_release);
        target.join();
        return false;
    }
    unblock.store(true, std::memory_order_release);
    if (!waitForCondition([&] { return unblocked.load(std::memory_order_acquire); }, 1s)) {
        spark::Capture::disarm();
        run.store(false, std::memory_order_release);
        target.join();
        return false;
    }
    spark::CaptureBuffer second;
    const bool recovered = spark::Capture::captureThread(tid.load(std::memory_order_acquire), second);
    run.store(false, std::memory_order_release);
    target.join();
    spark::Capture::disarm();
    if (!recovered || second.count == 0) {
        std::fprintf(stderr, "delayed signal: stale delivery prevented the next capture\n");
        return false;
    }

    tid.store(0, std::memory_order_release);
    unblock.store(false, std::memory_order_release);
    unblocked.store(false, std::memory_order_release);
    run.store(true, std::memory_order_release);
    target = start_blocked_target(tid, unblock, unblocked, run);
    if (!waitForCondition([&] { return tid.load(std::memory_order_acquire) != 0; }, 1s) || !spark::Capture::arm()) {
        unblock.store(true, std::memory_order_release);
        run.store(false, std::memory_order_release);
        target.join();
        return false;
    }
    spark::CaptureBuffer pending;
    const bool timed_out = !spark::Capture::captureThread(tid.load(std::memory_order_acquire), pending);
    spark::Capture::disarm();
    unblock.store(true, std::memory_order_release);
    run.store(false, std::memory_order_release);
    target.join();
    return timed_out;
}
#endif

#ifdef _WIN32
bool verifyWindowsThreadActivityDetection()
{
    using namespace std::chrono_literals;

    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> active_tid{0};
    std::atomic<std::uint64_t> sleeping_tid{0};
    std::atomic<std::uint64_t> work{0};
    HANDLE release_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (release_event == nullptr) {
        std::fprintf(stderr, "Windows thread activity: event creation failed\n");
        return false;
    }

    std::thread active([&] {
        active_tid.store(static_cast<std::uint64_t>(::GetCurrentThreadId()));
        while (run.load(std::memory_order_relaxed)) {
            work.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread sleeping([&] {
        sleeping_tid.store(static_cast<std::uint64_t>(::GetCurrentThreadId()));
        ::WaitForSingleObject(release_event, INFINITE);
    });
    while (active_tid.load() == 0 || sleeping_tid.load() == 0) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(20ms);

    auto finish = [&] {
        spark::Capture::disarm();
        run.store(false, std::memory_order_relaxed);
        ::SetEvent(release_event);
        active.join();
        sleeping.join();
        ::CloseHandle(release_event);
    };

    if (!spark::Capture::arm()) {
        std::fprintf(stderr, "Windows thread activity: capture arm failed\n");
        finish();
        return false;
    }
    const bool active_baseline = spark::Capture::isThreadRunning(active_tid.load());
    const bool sleeping_baseline = spark::Capture::isThreadRunning(sleeping_tid.load());
    std::this_thread::sleep_for(40ms);
    const bool active_running = spark::Capture::isThreadRunning(active_tid.load());
    const bool sleeping_running = spark::Capture::isThreadRunning(sleeping_tid.load());
    spark::Capture::disarm();

    if (!spark::Capture::arm()) {
        std::fprintf(stderr, "Windows thread activity: capture re-arm failed\n");
        finish();
        return false;
    }
    const bool restarted_baseline = spark::Capture::isThreadRunning(active_tid.load());
    finish();

    if (active_baseline || sleeping_baseline || !active_running || sleeping_running || restarted_baseline) {
        std::fprintf(stderr, "Windows thread activity: cycle-time classification failed\n");
        return false;
    }
    return true;
}
#endif

}  // namespace spark::selftest
