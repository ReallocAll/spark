#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "application/profiler/profiler_timeout.h"

namespace {

using namespace std::chrono_literals;

struct Probe {
    std::condition_variable cv;
    std::mutex mutex;
    int calls = 0;
    std::chrono::steady_clock::duration elapsed{};
};

template <typename Predicate>
bool waitFor(Probe &probe, Predicate predicate)
{
    std::unique_lock lock(probe.mutex);
    return probe.cv.wait_for(lock, 2s, std::move(predicate));
}

void testFiringAndSteadyElapsed()
{
    Probe probe;
    const auto start = std::chrono::steady_clock::now();
    spark::ProfilerTimeout timeout;
    assert(timeout.arm(60ms, [&probe, start] {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.elapsed = elapsed;
        probe.cv.notify_all();
    }));
    assert(waitFor(probe, [&probe] { return probe.calls == 1; }));
    timeout.cancel();

    std::scoped_lock lock(probe.mutex);
    assert(probe.elapsed >= 20ms);
}

void testCancelBeforeDeadline()
{
    Probe probe;
    spark::ProfilerTimeout timeout;
    assert(timeout.arm(250ms, [&probe] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
    }));
    std::this_thread::sleep_for(20ms);
    timeout.cancel();

    std::scoped_lock lock(probe.mutex);
    assert(probe.calls == 0);
}

void testRearmSuppressesOldCallback()
{
    Probe probe;
    bool old_called = false;
    bool new_called = false;
    spark::ProfilerTimeout timeout;
    assert(timeout.arm(250ms, [&probe, &old_called] {
        std::scoped_lock lock(probe.mutex);
        old_called = true;
        probe.cv.notify_all();
    }));
    assert(timeout.arm(0ms, [&probe, &new_called] {
        std::scoped_lock lock(probe.mutex);
        new_called = true;
        ++probe.calls;
        probe.cv.notify_all();
    }));
    assert(waitFor(probe, [&probe] { return probe.calls == 1; }));
    timeout.cancel();

    std::scoped_lock lock(probe.mutex);
    assert(!old_called);
    assert(new_called);
}

void testCallbackExceptionIsContained()
{
    Probe probe;
    spark::ProfilerTimeout timeout;
    assert(timeout.arm(0ms, [&probe] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
        throw std::runtime_error("timeout callback failure");
    }));
    assert(waitFor(probe, [&probe] { return probe.calls == 1; }));

    assert(timeout.arm(0ms, [&probe] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
    }));
    assert(waitFor(probe, [&probe] { return probe.calls == 2; }));
    timeout.cancel();
}

void testDestructorCancels()
{
    Probe probe;
    {
        spark::ProfilerTimeout timeout;
        assert(timeout.arm(250ms, [&probe] {
            std::scoped_lock lock(probe.mutex);
            ++probe.calls;
            probe.cv.notify_all();
        }));
    }
    std::this_thread::sleep_for(300ms);

    std::scoped_lock lock(probe.mutex);
    assert(probe.calls == 0);
}

void testMaximumDelayCanBeCancelled()
{
    Probe probe;
    spark::ProfilerTimeout timeout;
    assert(timeout.arm(std::chrono::milliseconds::max(), [&probe] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
    }));
    timeout.cancel();

    std::scoped_lock lock(probe.mutex);
    assert(probe.calls == 0);
}

void testSelfCancelDoesNotTerminate()
{
    Probe probe;
    spark::ProfilerTimeout timeout;
    assert(timeout.arm(0ms, [&probe, &timeout] {
        timeout.cancel();
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
    }));
    assert(waitFor(probe, [&probe] { return probe.calls == 1; }));
    timeout.cancel();
}

}  // namespace

int main()
{
    testFiringAndSteadyElapsed();
    testCancelBeforeDeadline();
    testRearmSuppressesOldCallback();
    testCallbackExceptionIsContained();
    testDestructorCancels();
    testMaximumDelayCanBeCancelled();
    testSelfCancelDoesNotTerminate();
    return 0;
}
