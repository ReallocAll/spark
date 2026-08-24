#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>

#include <sys/syscall.h>

#include "native/sampler/capture.h"

namespace spark {

struct CaptureTestAccess {
    static void setHandlerGate(std::atomic<bool> *entered, std::atomic<bool> *release)
    {
        Capture::setHandlerGateForTesting(entered, release);
    }

    static void setHandlerWakeGate(std::atomic<bool> *entered, std::atomic<bool> *release)
    {
        Capture::setHandlerWakeGateForTesting(entered, release);
    }

    static void setNextToken(std::uintptr_t next_token) { Capture::setNextTokenForTesting(next_token); }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;

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

class TargetThread {
public:
    explicit TargetThread(bool block_signal) : block_signal_(block_signal), thread_([this] { run(); }) {}

    ~TargetThread()
    {
        unblock_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        thread_.join();
    }

    std::uint64_t id() const { return tid_.load(std::memory_order_acquire); }

    bool ready() const { return tid_.load(std::memory_order_acquire) != 0; }

    void unblock() { unblock_.store(true, std::memory_order_release); }

    bool unblocked() const { return unblocked_.load(std::memory_order_acquire); }

private:
    void run()
    {
        sigset_t blocked{};
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPROF);
        if (block_signal_) {
            pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
        }
        tid_.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)), std::memory_order_release);
        bool signal_blocked = block_signal_;
        while (running_.load(std::memory_order_acquire)) {
            if (signal_blocked && unblock_.load(std::memory_order_acquire)) {
                pthread_sigmask(SIG_UNBLOCK, &blocked, nullptr);
                signal_blocked = false;
                unblocked_.store(true, std::memory_order_release);
            }
            work_.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        if (signal_blocked) {
            pthread_sigmask(SIG_UNBLOCK, &blocked, nullptr);
        }
    }

    bool block_signal_;
    std::atomic<bool> running_{true};
    std::atomic<bool> unblock_{false};
    std::atomic<bool> unblocked_{false};
    std::atomic<std::uint64_t> tid_{0};
    std::atomic<std::uint64_t> work_{0};
    std::thread thread_;
};

bool require(bool condition, const char *message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "capture lifecycle: %s\n", message);
    return false;
}

bool testConcurrentAdmission()
{
    TargetThread target(false);
    if (!require(waitFor([&] { return target.ready(); }, 1s), "target thread did not start")) {
        return false;
    }
    if (!require(spark::Capture::arm(), "arm failed for concurrent admission")) {
        return false;
    }

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    spark::CaptureTestAccess::setHandlerGate(&entered, &release);
    spark::CaptureBuffer first_buffer;
    std::atomic<bool> first_result{false};
    std::thread first([&] {
        first_result.store(spark::Capture::captureThread(target.id(), first_buffer), std::memory_order_release);
    });
    if (!require(waitFor([&] { return entered.load(std::memory_order_acquire); }, 1s),
                 "handler did not enter for concurrent admission")) {
        release.store(true, std::memory_order_release);
        first.join();
        spark::CaptureTestAccess::setHandlerGate(nullptr, nullptr);
        spark::Capture::disarm();
        return false;
    }

    spark::CaptureBuffer second_buffer;
    const bool second_result = spark::Capture::captureThread(target.id(), second_buffer);
    const bool first_disarm = spark::Capture::disarm();
    const bool arm_during_cleanup = spark::Capture::arm();
    release.store(true, std::memory_order_release);
    first.join();
    const bool capture_during_cleanup = spark::Capture::captureThread(target.id(), second_buffer);
    spark::CaptureTestAccess::setHandlerGate(nullptr, nullptr);
    const bool disarmed = spark::Capture::disarm();
    return require(!second_result, "concurrent capture was admitted") &&
           require(!first_disarm, "disarm succeeded while a capture was admitted") &&
           require(!arm_during_cleanup && !capture_during_cleanup, "cleanup accepted a new capture") &&
           require(first_result.load(std::memory_order_acquire) && first_buffer.count > 0,
                   "admitted capture did not complete") &&
           require(disarmed, "disarm failed after admitted capture completed");
}

bool testDelayedDelivery()
{
    TargetThread target(true);
    if (!require(waitFor([&] { return target.ready(); }, 1s), "blocked target thread did not start")) {
        return false;
    }
    if (!require(spark::Capture::arm(), "arm failed for delayed delivery")) {
        return false;
    }
    spark::CaptureBuffer stale_buffer;
    const auto start = std::chrono::steady_clock::now();
    const bool stale_result = spark::Capture::captureThread(target.id(), stale_buffer);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!require(!stale_result, "blocked signal unexpectedly completed") ||
        !require(elapsed >= 900ms && elapsed < 1500ms, "requested deadline was not monotonic and bounded")) {
        target.unblock();
        waitFor([&] { return target.unblocked(); }, 1s);
        spark::Capture::disarm();
        return false;
    }

    target.unblock();
    if (!require(waitFor([&] { return target.unblocked(); }, 1s), "blocked target did not unblock")) {
        spark::Capture::disarm();
        return false;
    }
    spark::CaptureBuffer fresh_buffer;
    const bool fresh_result = spark::Capture::captureThread(target.id(), fresh_buffer);
    const bool disarmed = spark::Capture::disarm();
    return require(fresh_result && fresh_buffer.count > 0, "stale delivery prevented a fresh capture") &&
           require(disarmed, "disarm failed after delayed delivery");
}

bool testQuiescenceRetry()
{
    TargetThread target(false);
    if (!require(waitFor([&] { return target.ready(); }, 1s), "target thread did not start")) {
        return false;
    }
    if (!require(spark::Capture::arm(), "arm failed for quiescence retry")) {
        return false;
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    spark::CaptureTestAccess::setHandlerGate(&entered, &release);
    spark::CaptureBuffer buffer;
    std::thread capture([&] { spark::Capture::captureThread(target.id(), buffer); });
    if (!require(waitFor([&] { return entered.load(std::memory_order_acquire); }, 1s),
                 "handler did not enter for quiescence retry")) {
        release.store(true, std::memory_order_release);
        capture.join();
        spark::CaptureTestAccess::setHandlerGate(nullptr, nullptr);
        spark::Capture::disarm();
        return false;
    }
    std::this_thread::sleep_for(2200ms);
    capture.join();

    const auto start = std::chrono::steady_clock::now();
    const bool first_disarm = spark::Capture::disarm();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const bool arm_during_cleanup = spark::Capture::arm();
    release.store(true, std::memory_order_release);
    const bool retry_disarm = spark::Capture::disarm();
    spark::CaptureTestAccess::setHandlerGate(nullptr, nullptr);
    const bool restarted = spark::Capture::arm();
    spark::CaptureBuffer restarted_buffer;
    const bool restarted_capture = restarted && spark::Capture::captureThread(target.id(), restarted_buffer);
    const bool final_disarm = spark::Capture::disarm();
    return require(!first_disarm, "disarm succeeded while handler remained gated") &&
           require(elapsed >= 900ms && elapsed < 1500ms, "quiescence deadline was not bounded") &&
           require(!arm_during_cleanup, "arm reinstalled during incomplete cleanup") &&
           require(retry_disarm, "disarm retry did not complete cleanup") &&
           require(restarted_capture && restarted_buffer.count > 0 && final_disarm,
                   "backend did not restart after cleanup retry");
}

bool testCompleteBeforeWake()
{
    TargetThread target(false);
    if (!require(waitFor([&] { return target.ready(); }, 1s), "target thread did not start")) {
        return false;
    }
    if (!require(spark::Capture::arm(), "arm failed for completion publication")) {
        return false;
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    spark::CaptureTestAccess::setHandlerWakeGate(&entered, &release);
    spark::CaptureBuffer buffer;
    std::atomic<bool> result{false};
    std::thread capture(
        [&] { result.store(spark::Capture::captureThread(target.id(), buffer), std::memory_order_release); });
    if (!require(waitFor([&] { return entered.load(std::memory_order_acquire); }, 1s),
                 "handler did not pause after completion publication")) {
        release.store(true, std::memory_order_release);
        capture.join();
        spark::CaptureTestAccess::setHandlerWakeGate(nullptr, nullptr);
        spark::Capture::disarm();
        return false;
    }
    capture.join();
    const bool first_disarm = spark::Capture::disarm();
    release.store(true, std::memory_order_release);
    const bool retry_disarm = spark::Capture::disarm();
    spark::CaptureTestAccess::setHandlerWakeGate(nullptr, nullptr);
    return require(result.load(std::memory_order_acquire) && buffer.count > 0,
                   "published completion was not observed") &&
           require(!first_disarm, "disarm ignored an active handler before wake") &&
           require(retry_disarm, "disarm retry failed after completion wake");
}

bool testTokenExhaustion()
{
    TargetThread target(false);
    if (!require(waitFor([&] { return target.ready(); }, 1s), "target thread did not start")) {
        return false;
    }
    if (!require(spark::Capture::arm(), "arm failed for token exhaustion")) {
        return false;
    }
    spark::CaptureTestAccess::setNextToken(std::numeric_limits<std::uintptr_t>::max());
    spark::CaptureBuffer buffer;
    const bool result = spark::Capture::captureThread(target.id(), buffer);
    spark::CaptureTestAccess::setNextToken(1);
    const bool disarmed = spark::Capture::disarm();
    return require(!result, "token exhaustion did not fail closed") &&
           require(disarmed, "disarm failed after token exhaustion");
}

}  // namespace

int main()
{
    if (!testConcurrentAdmission() || !testDelayedDelivery() || !testQuiescenceRetry() || !testCompleteBeforeWake() ||
        !testTokenExhaustion()) {
        return 1;
    }
    for (int i = 0; i < 3; ++i) {
        if (!require(spark::Capture::arm(), "repeated arm failed") ||
            !require(spark::Capture::disarm(), "repeated disarm failed")) {
            return 1;
        }
    }
    return 0;
}
