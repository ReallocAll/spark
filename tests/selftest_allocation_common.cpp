#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "selftest_allocation_internal.h"

namespace spark::selftest {

#define SPARK_NOINLINE __attribute__((noinline))
SPARK_NOINLINE bool exerciseNativeAllocations()
{
    for (std::size_t i = 0; i < 4096; ++i) {
        const std::size_t size = 512 + (i & 255);
        void *allocation = std::malloc(size);
        if (allocation == nullptr) {
            return false;
        }
        static_cast<volatile unsigned char *>(allocation)[0] = static_cast<unsigned char>(i);
        std::free(allocation);
    }
    void *resized = std::malloc(1024);
    if (resized == nullptr) {
        return false;
    }
    void *replacement = std::realloc(resized, 4096);
    if (replacement == nullptr) {

        std::free(resized);
        return false;
    }
    std::free(replacement);

    void *array = ::reallocarray(nullptr, 32, 32);
    if (array == nullptr) {
        return false;
    }
    void *array_replacement = ::reallocarray(array, 64, 32);
    if (array_replacement == nullptr) {
        std::free(array);
        return false;
    }
    std::free(array_replacement);
    void *cross_thread = std::malloc(4096);
    if (cross_thread == nullptr) {
        return false;
    }
    std::thread releaser([cross_thread]() { std::free(cross_thread); });
    releaser.join();
    return true;
}
#undef SPARK_NOINLINE

void allocationBurst(int count)
{
    for (int i = 0; i < count; ++i) {
        void *pointer = std::malloc(512U + static_cast<std::size_t>(i));
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
            std::free(pointer);
        }
    }
}

bool allocationTreesHaveOnly(const spark::AllocationSampler &sampler,
                             const std::vector<std::string_view> &expected_names)
{
    std::vector<bool> found(expected_names.size(), false);
    for (const auto &[id, thread] : sampler.threadTrees()) {
        bool allowed = false;
        for (std::size_t i = 0; i < expected_names.size(); ++i) {
            if (thread.thread_name.starts_with(std::string(expected_names[i]) + " (#")) {
                found[i] = true;
                allowed = true;
                break;
            }
        }
        if (!allowed || id == 0 || thread.tree.empty()) {
            return false;
        }
    }
    return std::ranges::all_of(found, [](bool value) { return value; });
}

bool runNamedAllocationWorkers(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                               const std::vector<const char *> &names, std::string &error)
{
    using namespace std::chrono_literals;
    if (!sampler.start(config, error)) {
        return false;
    }
    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> workers;
    workers.reserve(names.size());
    for (const char *name : names) {
        workers.emplace_back([&, name] {
            if (!setCurrentThreadName(name)) {
                ready.fetch_add(1000, std::memory_order_release);
                return;
            }
            allocationBurst();
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < static_cast<int>(names.size())) {
        std::this_thread::yield();
    }
    const bool named = ready.load(std::memory_order_acquire) < 1000;
    std::this_thread::sleep_for(30ms);
    release.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }
    sampler.onTick(50.0);
    const bool stopped = sampler.stop(error);
    return named && stopped;
}

bool runAllocationSession(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                          std::string &error)
{
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation lifecycle: start failed: %s\n", error.c_str());
        return false;
    }
    if (!exerciseNativeAllocations()) {
        std::fprintf(stderr, "allocation lifecycle: test allocation failed\n");
        return false;
    }
    sampler.onTick(50.0);
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "allocation lifecycle: stop failed: %s\n", error.c_str());
        return false;
    }
    if (sampler.sampleCount() == 0 || sampler.observedBytes() == 0 || sampler.freedSamples() == 0 ||
        sampler.freedBytes() == 0 || (sampler.lifecycleDropped() != 0 && !sampler.dataIncomplete())) {
        std::fprintf(stderr,
                     "allocation lifecycle: invalid counters "
                     "(samples=%llu observed=%llu freed=%llu freed-bytes=%llu "
                     "lifecycle-dropped=%llu)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()),
                     static_cast<unsigned long long>(sampler.observedBytes()),
                     static_cast<unsigned long long>(sampler.freedSamples()),
                     static_cast<unsigned long long>(sampler.freedBytes()),
                     static_cast<unsigned long long>(sampler.lifecycleDropped()));
        return false;
    }
    return true;
}
}  // namespace spark::selftest
