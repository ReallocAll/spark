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

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace spark::selftest {

bool setCurrentThreadName(const char *name)
{
#ifdef _WIN32
    int length = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (length <= 1) {
        return false;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(length));
    if (::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), length) == 0) {
        return false;
    }
    return SUCCEEDED(::SetThreadDescription(::GetCurrentThread(), wide.data()));
#elif defined(__linux__)
    return ::pthread_setname_np(::pthread_self(), name) == 0;
#else
    (void)name;
    return false;
#endif
}

#if defined(_WIN32) || defined(__linux__)
#ifdef _WIN32
#define SPARK_NOINLINE __declspec(noinline)
#else
#define SPARK_NOINLINE __attribute__((noinline))
#endif
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

#ifdef _WIN32
    void *recalloced = _recalloc(nullptr, 32, 32);
    if (recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _recalloc failed\n");
        return false;
    }
    void *recalloced_replacement = _recalloc(recalloced, 64, 32);
    if (recalloced_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: resized _recalloc failed\n");
        std::free(recalloced);
        return false;
    }
    std::free(recalloced_replacement);

    void *aligned = _aligned_malloc(1024, 64);
    if (aligned == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_malloc failed\n");
        return false;
    }
    void *aligned_replacement = _aligned_realloc(aligned, 4096, 64);
    if (aligned_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_realloc failed\n");
        _aligned_free(aligned);
        return false;
    }
    void *aligned_recalloced = _aligned_recalloc(aligned_replacement, 128, 64, 64);
    if (aligned_recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_recalloc failed\n");
        _aligned_free(aligned_replacement);
        return false;
    }
    _aligned_free(aligned_recalloced);

    void *offset = _aligned_offset_malloc(1024, 64, 16);
    if (offset == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_malloc failed\n");
        return false;
    }
    void *offset_replacement = _aligned_offset_realloc(offset, 4096, 64, 16);
    if (offset_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_realloc failed\n");
        _aligned_free(offset);
        return false;
    }
    void *offset_recalloced = _aligned_offset_recalloc(offset_replacement, 128, 64, 64, 16);
    if (offset_recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_recalloc failed\n");
        _aligned_free(offset_replacement);
        return false;
    }
    _aligned_free(offset_recalloced);
#elif defined(__linux__)
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
#endif

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
#endif

}  // namespace spark::selftest
