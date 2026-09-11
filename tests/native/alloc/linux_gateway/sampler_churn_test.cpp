#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "native/alloc/allocation_sampler.h"

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::abort();
    }
}

void allocation()
{
    void *(*volatile allocate)(std::size_t) = &std::malloc;
    void (*volatile release)(void *) = &std::free;
    void *pointer = allocate(4096);
    require(pointer != nullptr, "native allocation");
    release(pointer);
}

}  // namespace

int main()
{
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1024;
    config.session_seed = 42;
    config.thread_state_limit_for_testing = 16;
    std::string error;
    for (unsigned cycle = 0; cycle < 6; ++cycle) {
        config.count_only = cycle % 3 == 1;
        require(sampler.start(config, error), error);
        for (unsigned iteration = 0; iteration < 128; ++iteration) {
            std::thread thread(allocation);
            thread.join();
        }
        std::barrier entered(5);
        std::barrier stopped(5);
        std::array<std::thread, 4> exiting;
        for (auto &thread : exiting) {
            thread = std::thread([&] {
                allocation();
                entered.arrive_and_wait();
                stopped.arrive_and_wait();
            });
        }
        entered.arrive_and_wait();
        require(sampler.stop(error), error);
        if (!config.count_only) {
            require(sampler.sampledThreadCount() > 16, "sampled-thread churn coverage");
        }
        stopped.arrive_and_wait();
        for (auto &thread : exiting) {
            thread.join();
        }
        require(sampler.threadStateDrops() == 0, "thread registry reclaimed across churn and stopped exits");
    }
    require(sampler.shutdown(error), error);
    require(sampler.start(config, error), error);
    allocation();
    require(sampler.stop(error) && sampler.shutdown(error), error);
    std::puts("PASS: real sampler full/count-only/restart/churn and stopped thread exits");
}
