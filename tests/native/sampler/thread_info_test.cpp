#include "native/sampler/thread_info.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>

int main()
{
    const std::uint64_t main_id = spark::currentNativeThreadId();
    assert(main_id != 0);
    for (int i = 0; i < 10000; ++i) {
        assert(spark::currentNativeThreadId() == main_id);
    }

    std::atomic<std::uint64_t> worker_id{0};
    std::thread worker([&] {
        const std::uint64_t first = spark::currentNativeThreadId();
        assert(first != 0);
        for (int i = 0; i < 10000; ++i) {
            assert(spark::currentNativeThreadId() == first);
        }
        worker_id.store(first, std::memory_order_release);
    });
    worker.join();

    const std::uint64_t observed_worker_id = worker_id.load(std::memory_order_acquire);
    assert(observed_worker_id != 0);
    assert(observed_worker_id != main_id);
    return 0;
}
