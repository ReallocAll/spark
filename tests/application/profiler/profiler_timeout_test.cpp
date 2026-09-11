#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
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
    const bool armed = timeout.arm(60ms, [&probe, start] {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.elapsed = elapsed;
        probe.cv.notify_all();
    });
    assert(armed);
    assert(waitFor(probe, [&probe] { return probe.calls == 1; }));
    timeout.cancel();

    std::scoped_lock lock(probe.mutex);
    assert(probe.elapsed >= 20ms);
}

void testCancelBeforeDeadline()
{
    Probe probe;
    spark::ProfilerTimeout timeout;
    const bool armed = timeout.arm(250ms, [&probe] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
    });
    assert(armed);
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
    const bool first_armed = timeout.arm(250ms, [&probe, &old_called] {
        std::scoped_lock lock(probe.mutex);
        old_called = true;
        probe.cv.notify_all();
    });
    assert(first_armed);
    const bool second_armed = timeout.arm(0ms, [&probe, &new_called] {
        std::scoped_lock lock(probe.mutex);
        new_called = true;
        ++probe.calls;
        probe.cv.notify_all();
    });
    assert(second_armed);
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
    const bool first_armed = timeout.arm(0ms, [&probe] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
        throw std::runtime_error("timeout callback failure");
    });
    assert(first_armed);
    assert(waitFor(probe, [&probe] { return probe.calls == 1; }));

    const bool second_armed = timeout.arm(0ms, [&probe] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
    });
    assert(second_armed);
    assert(waitFor(probe, [&probe] { return probe.calls == 2; }));
    timeout.cancel();
}

void testDestructorCancels()
{
    Probe probe;
    {
        spark::ProfilerTimeout timeout;
        const bool armed = timeout.arm(250ms, [&probe] {
            std::scoped_lock lock(probe.mutex);
            ++probe.calls;
            probe.cv.notify_all();
        });
        assert(armed);
    }
    std::this_thread::sleep_for(300ms);

    std::scoped_lock lock(probe.mutex);
    assert(probe.calls == 0);
}

void testMaximumDelayCanBeCancelled()
{
    Probe probe;
    spark::ProfilerTimeout timeout;
    const bool armed = timeout.arm(std::chrono::milliseconds::max(), [&probe] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
    });
    assert(armed);
    timeout.cancel();

    std::scoped_lock lock(probe.mutex);
    assert(probe.calls == 0);
}

void testSelfCancelDoesNotTerminate()
{
    Probe probe;
    spark::ProfilerTimeout timeout;
    const bool armed = timeout.arm(0ms, [&probe, &timeout] {
        timeout.cancel();
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
    });
    assert(armed);
    assert(waitFor(probe, [&probe] { return probe.calls == 1; }));
    timeout.cancel();
}

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    void block()
    {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return release; });
    }
    void waitEntered()
    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 2s, [this] { return entered; }));
    }
    void unblock()
    {
        std::scoped_lock lock(mutex);
        release = true;
        cv.notify_all();
    }
};

struct ThreadExitGate {
    Gate *gate = nullptr;
    ~ThreadExitGate()
    {
        if (gate) {
            gate->block();
        }
    }
};

void testNativeExitRetainsCapture(bool block_thread_exit)
{
    Gate gate;
    spark::ProfilerTimeout timeout;
    auto capture = std::make_shared<int>(42);
    const std::weak_ptr<int> weak = capture;
    const bool armed = timeout.arm(0ms, [capture = std::move(capture), &gate, block_thread_exit] {
        assert(*capture == 42);
        if (block_thread_exit) {
            thread_local ThreadExitGate exit_gate;
            exit_gate.gate = &gate;
        }
        else {
            gate.block();
        }
    });
    assert(armed);
    gate.waitEntered();
    const auto begin = std::chrono::steady_clock::now();
    assert(!timeout.cancelUntil(begin + 20ms));
    assert(std::chrono::steady_clock::now() - begin < 200ms);
    assert(!weak.expired());
    assert(!timeout.reapUntil(std::chrono::steady_clock::now()));
    assert(!timeout.arm(0ms, [] {}));
    gate.unblock();
    assert(timeout.reapUntil(std::chrono::steady_clock::now() + 2s));
    assert(weak.expired());
    std::atomic<int> fired{0};
    const bool rearmed = timeout.arm(0ms, [&] { ++fired; });
    assert(rearmed);
    assert(timeout.reapUntil(std::chrono::steady_clock::now() + 2s));
    assert(fired.load() == 1);
}

void testSelfCancelWhileExternallyReaping()
{
    Gate gate;
    spark::ProfilerTimeout timeout;
    std::atomic<bool> self_done{false};
    assert(timeout.arm(0ms, [&] {
        gate.block();
        const auto begin = std::chrono::steady_clock::now();
        const std::function<void()> inline_dispatch = [&] {
            assert(!timeout.cancelUntil(std::chrono::steady_clock::now() + 2s));
            assert(!timeout.arm(0ms, [] {}));
        };
        inline_dispatch();
        assert(std::chrono::steady_clock::now() - begin < 200ms);
        self_done.store(true);
    }));
    gate.waitEntered();
    std::atomic<bool> reaper_entered{false};
    std::thread reaper([&] {
        reaper_entered.store(true);
        assert(timeout.reapUntil(std::chrono::steady_clock::now() + 2s));
    });
    while (!reaper_entered.load()) {
        std::this_thread::yield();
    }
    gate.unblock();
    reaper.join();
    assert(self_done.load());
    assert(timeout.cancelUntil(std::chrono::steady_clock::now()));
}

int selfDestroyChild()
{
    std::set_terminate([] { std::_Exit(91); });
    auto *timeout = new spark::ProfilerTimeout();
    const bool armed = timeout->arm(10ms, [timeout] {
        std::set_terminate([] { std::_Exit(91); });
        delete timeout;
    });
    assert(armed);
    std::this_thread::sleep_for(2s);
    return 1;
}

void testReapDoesNotCancelArmedTimer()
{
    Probe probe;
    spark::ProfilerTimeout timeout;
    assert(!timeout.arm(0ms, {}));
    const bool armed = timeout.arm(60ms, [&] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
    });
    assert(armed);
    assert(!timeout.reapUntil(std::chrono::steady_clock::now()));
    assert(waitFor(probe, [&] { return probe.calls == 1; }));
    assert(timeout.reapUntil(std::chrono::steady_clock::now() + 2s));
    const bool rearmed = timeout.arm(-20ms, [&] {
        std::scoped_lock lock(probe.mutex);
        ++probe.calls;
        probe.cv.notify_all();
    });
    assert(rearmed);
    assert(waitFor(probe, [&] { return probe.calls == 2; }));
    assert(timeout.reapUntil(std::chrono::steady_clock::now() + 2s));
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--self-destroy") {
        return selfDestroyChild();
    }
    testFiringAndSteadyElapsed();
    testCancelBeforeDeadline();
    testRearmSuppressesOldCallback();
    testCallbackExceptionIsContained();
    testDestructorCancels();
    testMaximumDelayCanBeCancelled();
    testSelfCancelDoesNotTerminate();
    testNativeExitRetainsCapture(false);
    testNativeExitRetainsCapture(true);
    testSelfCancelWhileExternallyReaping();
    testReapDoesNotCancelArmedTimer();
    return 0;
}
