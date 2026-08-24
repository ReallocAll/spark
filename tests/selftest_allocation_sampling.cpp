#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/profiler/profiler.h"
#include "native/alloc/byte_sampler.h"
#include "proto/sampler_data.h"
#include "selftest_allocation_internal.h"

#ifdef _WIN32
#include <malloc.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

namespace spark::selftest {

bool verifyByteSampling()
{
    constexpr std::uint64_t seed = 0x7f4a7c159e3779b9ULL;
    spark::ByteSamplingState first;
    spark::ByteSamplingState replay;

    spark::resetByteSamplingState(first, 1, seed, 1);
    if (spark::consumeSampledBytes(first, 100000, 1) != 100000) {
        std::fprintf(stderr, "byte sampling: interval=1 was not exact\n");
        return false;
    }

    spark::resetByteSamplingState(first, 1, seed, 64);
    first.bytes_until_sample = 7;
    constexpr std::uint64_t large_request = 1'000'000'000'033ULL;
    constexpr std::uint64_t expected_points = 1 + (large_request - 7) / 64;
    if (spark::consumeSampledBytes(first, large_request, 64) != expected_points ||
        first.bytes_until_sample != 64 - ((large_request - 7) % 64)) {
        std::fprintf(stderr, "byte sampling: large allocation crossing count was incorrect\n");
        return false;
    }

    spark::resetByteSamplingState(first, 2, seed, 64);
    spark::resetByteSamplingState(replay, 2, seed, 64);
    for (int i = 0; i < 1000; ++i) {
        const auto bytes = static_cast<std::uint64_t>((i * 7919) % 4096 + 1);
        if (spark::consumeSampledBytes(first, bytes, 64) != spark::consumeSampledBytes(replay, bytes, 64)) {
            std::fprintf(stderr, "byte sampling: identical session seed did not replay\n");
            return false;
        }
    }

    for (const std::uint64_t interval : {4ULL, 64ULL, 1024ULL}) {
        spark::ByteSamplingState state;
        spark::resetByteSamplingState(state, interval, seed ^ interval, interval);
        constexpr std::uint64_t observed = 4'000'000;
        std::uint64_t points = 0;
        for (std::uint64_t consumed = 0; consumed < observed; consumed += 4096) {
            const std::uint64_t chunk = (std::min)(std::uint64_t{4096}, observed - consumed);
            points += spark::consumeSampledBytes(state, chunk, interval);
        }
        const double ratio =
            static_cast<double>(points) * static_cast<double>(interval) / static_cast<double>(observed);
        if (ratio < 0.94 || ratio > 1.06 || state.bytes_until_sample == 0) {
            std::fprintf(stderr, "byte sampling: interval=%llu produced implausible ratio %.6f\n",
                         static_cast<unsigned long long>(interval), ratio);
            return false;
        }
    }
    return true;
}

#if defined(_WIN32) || defined(__linux__)
#ifdef _WIN32
namespace {

void __cdecl ignoreInvalidParameter(const wchar_t * /*unused*/, const wchar_t * /*unused*/, const wchar_t * /*unused*/,
                                    unsigned int /*unused*/, std::uintptr_t /*unused*/)
{
}

}  // namespace
#endif

bool verifyProcessWideAllocationSampling()
{
    using namespace std::chrono_literals;

    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = spark::currentNativeThreadId();

    spark::AllocationSampler sampler;
    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "process-wide allocation: start failed: %s\n", error.c_str());
        return false;
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    auto allocate_on_worker = [&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 64; ++i) {
            void *pointer = std::malloc(512U + static_cast<std::size_t>(i));
            if (pointer != nullptr) {
                static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
                std::free(pointer);
            }
        }
    };
    std::thread first(allocate_on_worker);
    std::thread second(allocate_on_worker);
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    first.join();
    second.join();

    std::atomic<void *> handoff{nullptr};
    std::thread allocator([&]() { handoff.store(std::malloc(4096), std::memory_order_release); });
    allocator.join();
    void *original = handoff.load(std::memory_order_acquire);
    if (original == nullptr) {
        std::fprintf(stderr, "process-wide allocation: handoff malloc failed\n");
        sampler.stop(error);
        return false;
    }
    std::thread resizer([&]() {
        void *replacement = std::realloc(original, 1024 * 1024);
        handoff.store(replacement != nullptr ? replacement : original, std::memory_order_release);
    });
    resizer.join();
    std::thread releaser([&]() { std::free(handoff.load(std::memory_order_acquire)); });
    releaser.join();

    void *failed = std::malloc(1024);

    if (failed == nullptr) {
        std::fprintf(stderr, "process-wide allocation: failure probe malloc failed\n");
        sampler.stop(error);
        return false;
    }
    const std::size_t impossible = std::numeric_limits<std::size_t>::max();
    volatile std::size_t impossible_runtime = impossible;
    void *failure = std::realloc(failed, impossible);
    if (failure != nullptr) {
        std::free(failure);
    }
    else {
        std::free(failed);
    }
#ifdef _WIN32
    _invalid_parameter_handler previous_handler = _set_thread_local_invalid_parameter_handler(ignoreInvalidParameter);
#endif
    void *calloc_overflow = std::calloc(impossible_runtime, 2);
    if (calloc_overflow != nullptr) {
        std::fprintf(stderr, "process-wide allocation: calloc overflow succeeded (%p)\n", calloc_overflow);
        std::free(calloc_overflow);
#ifdef _WIN32
        _set_thread_local_invalid_parameter_handler(previous_handler);
#endif
        sampler.stop(error);
        return false;
    }
#ifdef _WIN32
    void *recalloc_overflow = _recalloc(nullptr, impossible_runtime, 2);
    _set_thread_local_invalid_parameter_handler(previous_handler);
#else
    void *recalloc_overflow = ::reallocarray(nullptr, impossible_runtime, 2);
#endif
    if (recalloc_overflow != nullptr) {
        std::fprintf(stderr, "process-wide allocation: recalloc overflow succeeded\n");
        std::free(recalloc_overflow);
        sampler.stop(error);
        return false;
    }
    void *from_null = std::realloc(nullptr, 2048);
    std::free(from_null);
    void *to_zero = std::malloc(2048);
    if (to_zero != nullptr) {
        void *zero_result = std::realloc(to_zero, 0);
        std::free(zero_result);
    }

    for (int i = 0; i < 300; ++i) {
        std::thread short_lived([]() {
            void *pointer = std::malloc(128);
            if (pointer != nullptr) {
                static_cast<volatile unsigned char *>(pointer)[0] = 1;
                std::free(pointer);
            }
        });
        short_lived.join();
    }

    sampler.onTick(50.0);
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "process-wide allocation: stop failed: %s\n", error.c_str());
        return false;
    }

    const auto &trees = sampler.threadTrees();
    const auto overflow = trees.find(0);
    if (sampler.sampleCount() == 0 || sampler.samplingPoints() == 0 || sampler.enqueuedSamples() == 0 ||
        sampler.eventQueueHighWaterMark() == 0 || sampler.freedSamples() == 0 ||
        trees.size() != spark::AllocationSampler::threadRootCapacity() || sampler.overflowThreadCount() < 40 ||
        sampler.overflowThreadCount() > 512 || overflow == trees.end() ||
        overflow->second.thread_name != "<other threads>" || overflow->second.tree.empty()) {
        std::fprintf(stderr,
                     "process-wide allocation: invalid coverage "
                     "(samples=%llu points=%llu enqueued=%llu high-water=%llu freed=%llu "
                     "threads=%zu overflow=%llu)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()),
                     static_cast<unsigned long long>(sampler.samplingPoints()),
                     static_cast<unsigned long long>(sampler.enqueuedSamples()),
                     static_cast<unsigned long long>(sampler.eventQueueHighWaterMark()),
                     static_cast<unsigned long long>(sampler.freedSamples()), trees.size(),
                     static_cast<unsigned long long>(sampler.overflowThreadCount()));
        return false;
    }
    for (const auto &[id, thread] : trees) {
        if (thread.thread_name.empty() || thread.tree.empty()) {
            std::fprintf(stderr, "process-wide allocation: empty thread root %llu\n",
                         static_cast<unsigned long long>(id));
            return false;
        }
    }
    std::vector<spark::ThreadTreeView> views;
    views.reserve(trees.size());
    for (const auto &[id, thread] : trees) {
        views.push_back({.name = thread.thread_name, .tree = &thread.tree});
    }
    std::unordered_map<spark::FrameKey, spark::ResolvedFrame, spark::FrameKeyHash> resolved;
    for (const spark::FrameKey &frame : spark::collectFrameKeys(views)) {
        resolved.emplace(frame, spark::ResolvedFrame{.class_name = "selftest", .method_name = "allocation"});
    }
    spark::ProfileMetadata metadata;
    metadata.mode = spark::ProfileMode::Allocation;
    metadata.all_threads = true;
    const std::string profile = spark::buildSamplerData(metadata, views, resolved);
    if (profile.find("<other threads>") == std::string::npos || profile.find("session #") == std::string::npos) {
        std::fprintf(stderr, "process-wide allocation: thread roots were not serialized\n");
        return false;
    }
    return sampler.shutdown(error);
}
#endif

}  // namespace spark::selftest
