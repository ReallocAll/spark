#include <atomic>
#include <cassert>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

#include "native/alloc/windows_callback_gate.h"

namespace {

using spark::WindowsCallbackLifetimeGate;

void basicLifecycle()
{
    WindowsCallbackLifetimeGate gate;
    assert(gate.closed());
    assert(gate.drained());
    assert(gate.activeCount() == 0);
    assert(gate.generation() == 0);

    assert(gate.open());
    assert(!gate.closed());
    assert(!gate.drained());
    assert(!gate.open());

    assert(gate.tryEnter());
    assert(gate.tryEnter());
    assert(gate.activeCount() == 2);

    const auto previous_generation = gate.generation();
    assert(gate.close());
    assert(gate.closed());
    assert(gate.generation() == previous_generation + 1);
    assert(!gate.tryEnter());
    assert(!gate.drained());
    assert(!gate.open());

    assert(gate.leave());
    assert(gate.activeCount() == 1);
    assert(gate.leave());
    assert(gate.drained());
    assert(!gate.leave());

    assert(gate.open());
    assert(gate.tryEnter());
    assert(gate.leave());
    assert(gate.close());
    assert(gate.drained());
}

void deterministicDrain()
{
    constexpr int KThreads = 32;
    WindowsCallbackLifetimeGate gate;
    assert(gate.open());

    std::latch entered{KThreads};
    std::latch release{1};
    std::atomic<int> admitted{0};
    std::vector<std::thread> threads;
    threads.reserve(KThreads);

    for (int index = 0; index < KThreads; ++index) {
        threads.emplace_back([&] {
            if (!gate.tryEnter()) {
                entered.count_down();
                return;
            }
            admitted.fetch_add(1, std::memory_order_relaxed);
            entered.count_down();
            release.wait();
            assert(gate.leave());
        });
    }

    entered.wait();
    assert(admitted.load(std::memory_order_relaxed) == KThreads);
    assert(gate.activeCount() == static_cast<std::uint32_t>(KThreads));
    assert(gate.close());
    assert(!gate.tryEnter());
    assert(!gate.drained());

    release.count_down();
    for (auto &thread : threads) {
        thread.join();
    }
    assert(gate.drained());
}

void closedAdmissionIsStable()
{
    constexpr int KThreads = 32;
    WindowsCallbackLifetimeGate gate;
    assert(gate.open());
    assert(gate.close());

    std::atomic<int> unexpectedly_admitted{0};
    std::vector<std::thread> threads;
    threads.reserve(KThreads);
    for (int index = 0; index < KThreads; ++index) {
        threads.emplace_back([&] {
            for (int iteration = 0; iteration < 10000; ++iteration) {
                if (gate.tryEnter()) {
                    unexpectedly_admitted.fetch_add(1, std::memory_order_relaxed);
                    assert(gate.leave());
                }
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    assert(unexpectedly_admitted.load(std::memory_order_relaxed) == 0);
    assert(gate.drained());
}

void repeatedGenerationsDoNotReuseOpenState()
{
    WindowsCallbackLifetimeGate gate;
    std::uint32_t previous_generation = gate.generation();

    for (int iteration = 0; iteration < 10000; ++iteration) {
        assert(gate.open());
        assert(gate.tryEnter());
        assert(gate.leave());
        assert(gate.close());
        assert(gate.drained());
        const std::uint32_t current_generation = gate.generation();
        assert(current_generation > previous_generation);
        previous_generation = current_generation;
    }
}

void concurrentStartStopStress()
{
    constexpr int KWorkers = 8;
    constexpr int KCycles = 2000;

    WindowsCallbackLifetimeGate gate;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> entries{0};
    std::vector<std::thread> workers;
    workers.reserve(KWorkers);

    for (int index = 0; index < KWorkers; ++index) {
        workers.emplace_back([&] {
            while (!done.load(std::memory_order_acquire)) {
                if (gate.tryEnter()) {
                    entries.fetch_add(1, std::memory_order_relaxed);
                    assert(gate.leave());
                }
                std::this_thread::yield();
            }
        });
    }

    for (int cycle = 0; cycle < KCycles; ++cycle) {
        assert(gate.open());
        for (int spin = 0; spin < 8; ++spin) {
            std::this_thread::yield();
        }
        assert(gate.close());
        while (!gate.drained()) {
            std::this_thread::yield();
        }
    }

    done.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }

    assert(gate.drained());
    assert(entries.load(std::memory_order_relaxed) > 0);
}

}  // namespace

int main()
{
    basicLifecycle();
    deterministicDrain();
    closedAdmissionIsStable();
    repeatedGenerationsDoNotReuseOpenState();
    concurrentStartStopStress();
    return 0;
}
