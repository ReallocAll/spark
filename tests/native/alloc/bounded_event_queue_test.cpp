#include <atomic>
#include <cassert>
#include <cstddef>
#include <thread>
#include <vector>

#include "native/alloc/bounded_event_queue.h"

int main()
{
    spark::BoundedEventQueue<int, 4> queue;
    static_assert(spark::BoundedEventQueue<int, 4>::capacity() == 4);

    for (int value = 0; value < 4; ++value) {
        assert(queue.enqueue(value));
    }
    assert(!queue.enqueue(4));

    for (int expected = 0; expected < 4; ++expected) {
        int value = -1;
        assert(queue.dequeue(value));
        assert(value == expected);
    }
    int value = -1;
    assert(!queue.dequeue(value));

    constexpr std::size_t producer_count = 8;
    spark::BoundedEventQueue<int, 2, 1> contended_queue;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<std::size_t> successes{0};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t index = 0; index < producer_count; ++index) {
        producers.emplace_back([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (contended_queue.enqueue(static_cast<int>(index))) {
                successes.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != producer_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread &producer : producers) {
        producer.join();
    }

    const std::size_t successful_enqueues = successes.load(std::memory_order_relaxed);
    assert(successful_enqueues <= contended_queue.capacity());
    std::size_t dequeued = 0;
    while (contended_queue.dequeue(value)) {
        ++dequeued;
    }
    assert(dequeued == successful_enqueues);
    return 0;
}
