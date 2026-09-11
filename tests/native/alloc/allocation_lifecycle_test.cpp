#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "native/alloc/allocation_lifecycle_test_access.h"

namespace {

using Clock = std::chrono::steady_clock;

template <typename Predicate>
bool waitFor(Predicate &&predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

spark::AllocationSamplerConfig makeConfig(bool count_only)
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 4096;
    config.session_seed = count_only ? 0x11112222ULL : 0x33334444ULL;
    config.count_only = count_only;
    return config;
}

class LifecycleFixture {
public:
    ~LifecycleFixture()
    {
        tracking_gate.release.store(true, std::memory_order_release);
        start_failure_gate.fail_now.store(true, std::memory_order_release);
        if (starter.joinable()) {
            starter.join();
        }
        if (holder.joinable()) {
            holder.join();
        }
        if (thread_creation_failure_armed) {
            spark::test::AllocationLifecycleTestAccess::disarmThreadCreationFailure(sampler);
        }
        std::string error;
        (void)sampler.shutdown(error);
    }

    spark::AllocationSampler sampler;
    spark::test::TrackingGate tracking_gate;
    spark::test::StartFailureGate start_failure_gate;
    std::thread holder;
    std::thread starter;
    bool thread_creation_failure_armed = false;
};

bool report(const char *message)
{
    std::fprintf(stderr, "allocation lifecycle: %s\n", message);
    return false;
}

bool verifyCountOnlyHeldTracking()
{
    LifecycleFixture fixture;
    const auto config = makeConfig(true);
    std::string error;
    if (!fixture.sampler.start(config, error)) {
        return report("count-only start failed");
    }
    if (!fixture.sampler.running() || fixture.sampler.aggregatorMayBeAlive() ||
        fixture.sampler.backendCleanupPending()) {
        return report("count-only session has unexpected lifecycle state");
    }

    fixture.holder = std::thread([&fixture] {
        (void)spark::test::AllocationLifecycleTestAccess::holdTrackingCall(fixture.sampler, fixture.tracking_gate);
    });
    if (!waitFor([&fixture] { return fixture.tracking_gate.entered.load(std::memory_order_acquire); },
                 std::chrono::seconds(2))) {
        return report("count-only tracking guard did not enter");
    }

    const auto stop_started = Clock::now();
    const bool stopped = fixture.sampler.stop(error);
    const auto stop_elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - stop_started);
    if (stopped || !fixture.sampler.backendCleanupPending() || fixture.sampler.aggregatorMayBeAlive() ||
        fixture.sampler.running() || stop_elapsed > std::chrono::seconds(8)) {
        return report("held count-only stop did not fail bounded with pending cleanup");
    }

    error.clear();
    if (fixture.sampler.stop(error) || !fixture.sampler.backendCleanupPending()) {
        return report("repeated held count-only stop falsely succeeded");
    }
    error.clear();
    if (fixture.sampler.start(config, error) || !fixture.sampler.backendCleanupPending()) {
        return report("count-only restart bypassed pending cleanup");
    }

    fixture.tracking_gate.release.store(true, std::memory_order_release);
    fixture.holder.join();
    if (!fixture.tracking_gate.exited.load(std::memory_order_acquire)) {
        return report("count-only tracking guard did not exit");
    }
    error.clear();
    if (!fixture.sampler.stop(error) || fixture.sampler.backendCleanupPending()) {
        return report("count-only cleanup retry failed");
    }
    error.clear();
    if (!fixture.sampler.start(config, error) || !fixture.sampler.stop(error)) {
        return report("count-only session did not restart after cleanup");
    }
    return true;
}

bool verifyThreadCreationFailureHeldTracking()
{
    const auto config = makeConfig(false);
    std::atomic<bool> start_done{false};
    bool started = false;
    std::string start_error;
    LifecycleFixture fixture;
    spark::test::AllocationLifecycleTestAccess::armThreadCreationFailure(fixture.sampler, fixture.start_failure_gate);
    fixture.thread_creation_failure_armed = true;

    fixture.starter = std::thread([&] {
        started = fixture.sampler.start(config, start_error);
        start_done.store(true, std::memory_order_release);
    });
    if (!waitFor(
            [&fixture] { return fixture.start_failure_gate.before_thread_creation.load(std::memory_order_acquire); },
            std::chrono::seconds(2))) {
        return report("thread-creation seam did not publish");
    }

    fixture.holder = std::thread([&fixture] {
        (void)spark::test::AllocationLifecycleTestAccess::holdTrackingCall(fixture.sampler, fixture.tracking_gate);
    });
    if (!waitFor([&fixture] { return fixture.tracking_gate.entered.load(std::memory_order_acquire); },
                 std::chrono::seconds(2))) {
        return report("full-session tracking guard did not enter");
    }
    fixture.start_failure_gate.fail_now.store(true, std::memory_order_release);
    if (!waitFor([&start_done] { return start_done.load(std::memory_order_acquire); }, std::chrono::seconds(8))) {
        return report("thread-creation failure did not return bounded");
    }
    fixture.starter.join();
    if (started || fixture.sampler.aggregatorMayBeAlive() || !fixture.sampler.backendCleanupPending() ||
        fixture.sampler.running()) {
        return report("failed full start lost pending cleanup without an aggregator");
    }

    std::string error;
    if (fixture.sampler.start(config, error) || !fixture.sampler.backendCleanupPending()) {
        return report("failed full start accepted a restart while cleanup was pending");
    }
    fixture.tracking_gate.release.store(true, std::memory_order_release);
    fixture.holder.join();
    spark::test::AllocationLifecycleTestAccess::disarmThreadCreationFailure(fixture.sampler);
    fixture.thread_creation_failure_armed = false;
    if (!fixture.sampler.stop(error) || fixture.sampler.backendCleanupPending()) {
        return report("failed full start cleanup retry failed");
    }
    error.clear();
    if (!fixture.sampler.start(config, error) || !fixture.sampler.stop(error)) {
        return report("full session did not restart after thread-creation cleanup");
    }
    return true;
}

bool verifyShutdownObligationSurvivesStop()
{
    LifecycleFixture fixture;
    const auto config = makeConfig(true);
    std::string error;
    if (!fixture.sampler.start(config, error)) {
        return report("shutdown-obligation start failed");
    }
    fixture.holder = std::thread([&fixture] {
        (void)spark::test::AllocationLifecycleTestAccess::holdTrackingCall(fixture.sampler, fixture.tracking_gate);
    });
    if (!waitFor([&fixture] { return fixture.tracking_gate.entered.load(std::memory_order_acquire); },
                 std::chrono::seconds(2))) {
        return report("shutdown-obligation tracking guard did not enter");
    }

    if (fixture.sampler.shutdown(error) || !fixture.sampler.backendCleanupPending() ||
        fixture.sampler.aggregatorMayBeAlive()) {
        return report("failed shutdown did not retain its obligation");
    }
#ifdef __linux__
    error.clear();
    const auto stop_started = Clock::now();
    const bool stopped = fixture.sampler.stop(error);
    const auto stop_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stop_started);
    const bool cleanup_pending = fixture.sampler.backendCleanupPending();
    const bool aggregator_alive = fixture.sampler.aggregatorMayBeAlive();
    const bool running = fixture.sampler.running();
    if (stopped || !cleanup_pending || aggregator_alive || running || stop_elapsed > std::chrono::seconds(8) ||
        error.find("shutdown") == std::string::npos) {
        std::fprintf(stderr,
                     "allocation lifecycle: held ordinary stop did not preserve shutdown admission "
                     "(stopped=%d pending=%d aggregator=%d running=%d elapsed_ms=%lld error=%s)\n",
                     static_cast<int>(stopped), static_cast<int>(cleanup_pending), static_cast<int>(aggregator_alive),
                     static_cast<int>(running), static_cast<long long>(stop_elapsed.count()), error.c_str());
        return false;
    }
#endif
    fixture.tracking_gate.release.store(true, std::memory_order_release);
    fixture.holder.join();

    error.clear();
    if (fixture.sampler.stop(error) || !fixture.sampler.backendCleanupPending() ||
        error.find("shutdown") == std::string::npos) {
        return report("ordinary stop forgot a pending shutdown obligation");
    }
    error.clear();
    if (fixture.sampler.start(config, error) || !fixture.sampler.backendCleanupPending()) {
        return report("restart bypassed a pending shutdown obligation");
    }
    error.clear();
    if (!fixture.sampler.shutdown(error) || fixture.sampler.backendCleanupPending()) {
        return report("shutdown retry did not clear its obligation");
    }
    error.clear();
    if (!fixture.sampler.start(config, error) || !fixture.sampler.stop(error) || !fixture.sampler.shutdown(error)) {
        return report("session did not restart after shutdown cleanup");
    }
    return true;
}

}  // namespace

int main()
{
    if (!verifyCountOnlyHeldTracking() || !verifyThreadCreationFailureHeldTracking() ||
        !verifyShutdownObligationSurvivesStop()) {
        return 1;
    }
    return 0;
}
