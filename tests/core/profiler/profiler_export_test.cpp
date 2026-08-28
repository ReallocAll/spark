#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>

#ifdef _WIN32
#include <malloc.h>
#endif

#include "core/profiler/profiler.h"
#include "native/sampler/thread_info.h"

namespace spark {

struct ProfilerTestAccess {
    static bool allocationSnapshot(Profiler &profiler, AllocationSnapshot &snapshot, std::string &error)
    {
        return profiler.allocation_sampler_.snapshot(snapshot, error);
    }

    static bool allocationSamplerRunning(const Profiler &profiler) { return profiler.allocation_sampler_.running(); }

    static bool allocationHooksInstalled(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.hooksInstalled();
    }

    static std::uint64_t allocationLifecycleDropped(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.lifecycleDropped();
    }

    static std::uint64_t allocationContentionDropped(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.contentionDropped();
    }

    static bool persistentAllocationCountingActive(const Profiler &profiler)
    {
        return profiler.persistent_allocation_counting_active_.load(std::memory_order_acquire);
    }
};

}  // namespace spark

namespace {

template <typename Predicate, typename Rep, typename Period>
bool waitForCondition(Predicate pred, std::chrono::duration<Rep, Period> timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
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

bool verifyPersistentAllocationExportOrdering()
{
    using namespace std::chrono_literals;

    spark::Profiler profiler;
    const std::uint64_t server_tid = spark::currentNativeThreadId();
    std::string error;
    if (!profiler.setPersistentAllocationCountingEnabled(true, server_tid, error) ||
        !spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: count-only setup failed: %s\n", error.c_str());
        return false;
    }

    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 1;
    if (!profiler.start(options, server_tid, error)) {
        std::fprintf(stderr, "persistent export: allocation start failed: %s\n", error.c_str());
        return false;
    }
    if (spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: count-only remained active during full profile\n");
        profiler.cancel(error);
        return false;
    }
    if (!exerciseNativeAllocations() ||
        !waitForCondition([&] { return profiler.sampleCount() != 0; }, 2s)) {
        std::fprintf(stderr, "persistent export: no allocation samples were collected\n");
        profiler.cancel(error);
        return false;
    }
    const std::uint64_t samples_before_stop = profiler.sampleCount();
    const std::uint64_t bytes_before_stop = profiler.sampledAllocationBytes();

    if (!profiler.stopSampling(error)) {
        std::fprintf(stderr, "persistent export: stop failed: %s\n", error.c_str());
        return false;
    }
    const std::uint64_t samples_after_stop = profiler.sampleCount();
    const std::uint64_t bytes_after_stop = profiler.sampledAllocationBytes();
    if (spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) || samples_after_stop == 0 ||
        bytes_after_stop == 0 || samples_after_stop < samples_before_stop || bytes_after_stop < bytes_before_stop) {
        std::fprintf(stderr,
                     "persistent export: completed profile was lost before export "
                     "(samples=%llu/%llu bytes=%llu/%llu active=%d)\n",
                     static_cast<unsigned long long>(samples_after_stop),
                     static_cast<unsigned long long>(samples_before_stop),
                     static_cast<unsigned long long>(bytes_after_stop),
                     static_cast<unsigned long long>(bytes_before_stop),
                     static_cast<int>(spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)));
        return false;
    }

    const std::string profile = profiler.exportData({});
    if (profile.empty() || profiler.sampleCount() != samples_after_stop ||
        profiler.sampledAllocationBytes() != bytes_after_stop) {
        std::fprintf(stderr, "persistent export: serialization did not preserve completed samples\n");
        return false;
    }
    if (!profiler.resumePersistentAllocationCounting(error) ||
        !spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: count-only resume failed: %s\n", error.c_str());
        return false;
    }
    return profiler.shutdown(error);
}

bool verifyRetainedAllocationProfile()
{
    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.alloc_live_only = true;
    options.allocation_interval_bytes = 1;
    std::string error;
    const std::uint64_t server_tid = spark::currentNativeThreadId();
    if (!profiler.start(options, server_tid, error)) {
        std::fprintf(stderr, "retained allocation: start failed: %s\n", error.c_str());
        return false;
    }

    void *retained = std::malloc(8192);
    void *released = std::malloc(4096);
    if (retained == nullptr || released == nullptr) {
        std::free(retained);
        std::free(released);
        return false;
    }
    static_cast<volatile unsigned char *>(retained)[0] = 1;
    static_cast<volatile unsigned char *>(released)[0] = 2;
    void *resized = std::realloc(retained, 16384);
    if (resized != nullptr) {
        retained = resized;
    }
    void *failed_resize = std::realloc(retained, std::numeric_limits<std::size_t>::max());
    if (failed_resize != nullptr) {
        std::free(failed_resize);
        std::free(released);
        return false;
    }
    static_cast<volatile unsigned char *>(retained)[0] = 3;
    std::free(released);
    profiler.onTick(50.0);
    if (!profiler.stopSampling(error)) {
        std::fprintf(stderr, "retained allocation: stop failed: %s\n", error.c_str());
        std::free(retained);
        return false;
    }

    spark::ExportContext context;
    const std::string profile = profiler.exportData(context);
    const bool valid = profiler.sampleCount() != 0 && profiler.sampledAllocationBytes() >= 8192 &&
                       profiler.freedAllocationSamples() != 0 &&
                       profile.find("Allocation live-only") != std::string::npos &&
                       profile.find("Allocation retained maximum age ms") != std::string::npos &&
                       profile.find("Allocation hook capabilities") != std::string::npos &&
                       profile.find("Allocation hook targets installed") != std::string::npos &&
                       profile.find("Allocation thread filter stage") != std::string::npos;
    std::free(retained);
    if (!profiler.shutdown(error) || !valid) {
        std::fprintf(stderr,
                     "retained allocation: profile validation failed: %s "
                     "(samples=%llu bytes=%llu freed=%llu live-meta=%d age-meta=%d)\n",
                     error.c_str(), static_cast<unsigned long long>(profiler.sampleCount()),
                     static_cast<unsigned long long>(profiler.sampledAllocationBytes()),
                     static_cast<unsigned long long>(profiler.freedAllocationSamples()),
                     static_cast<int>(profile.find("Allocation live-only") != std::string::npos),
                     static_cast<int>(profile.find("Allocation retained maximum age ms") != std::string::npos));
        return false;
    }
    return true;
}

bool verifyAllocationLiveExport()
{
    using namespace std::chrono_literals;

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 4096;
    std::string error;
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "allocation live export: start failed: %s\n", error.c_str());
        return false;
    }

    void *first_allocation = std::malloc(4096);
    if (first_allocation == nullptr) {
        profiler.cancel(error);
        return false;
    }
    static_cast<volatile unsigned char *>(first_allocation)[0] = 1;
    if (!waitForCondition([&] { return profiler.sampleCount() != 0; }, 2s)) {
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }

    spark::AllocationSnapshot first;
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, first, error) || first.sample_count == 0 ||
        first.sampled_bytes == 0) {
        std::fprintf(stderr, "allocation live export: first snapshot failed: %s\n", error.c_str());
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }

    void *second_allocation = std::malloc(8192);
    if (second_allocation == nullptr) {
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }
    static_cast<volatile unsigned char *>(second_allocation)[0] = 2;
    if (!waitForCondition([&] { return profiler.sampleCount() > first.sample_count; }, 2s)) {
        std::free(second_allocation);
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }

    spark::AllocationSnapshot second;
    const std::string live_profile = profiler.liveExport({});
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, second, error) || live_profile.empty() ||
        live_profile.find("Allocation backend") == std::string::npos || second.sample_count < first.sample_count ||
        second.sampled_bytes < first.sampled_bytes || !spark::ProfilerTestAccess::allocationSamplerRunning(profiler) ||
        !spark::ProfilerTestAccess::allocationHooksInstalled(profiler)) {
        std::fprintf(stderr, "allocation live export: cumulative snapshot or sampler state was invalid\n");
        std::free(second_allocation);
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }

    if (!profiler.stopSampling(error)) {
        std::free(second_allocation);
        std::free(first_allocation);
        return false;
    }
    const std::string final_profile = profiler.exportData({});
    const bool valid = !final_profile.empty() && profiler.sampleCount() >= second.sample_count &&
                       profiler.sampledAllocationBytes() >= second.sampled_bytes &&
                       spark::ProfilerTestAccess::allocationHooksInstalled(profiler);
    std::free(second_allocation);
    std::free(first_allocation);
    return profiler.shutdown(error) && valid;
}

bool verifyRetainedAllocationLiveExport()
{
    using namespace std::chrono_literals;

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.alloc_live_only = true;
    options.allocation_interval_bytes = 1;
    std::string error;
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "retained live export: start failed: %s\n", error.c_str());
        return false;
    }

    void *retained = std::malloc(1024 * 1024);
    void *released = std::malloc(512 * 1024);
    if (retained == nullptr || released == nullptr) {
        std::free(retained);
        std::free(released);
        profiler.cancel(error);
        return false;
    }
    static_cast<volatile unsigned char *>(retained)[0] = 1;
    static_cast<volatile unsigned char *>(released)[0] = 2;
    if (!waitForCondition([&] { return profiler.liveAllocationSamples() >= 2; }, 2s)) {
        std::fprintf(stderr, "retained live export: allocations were not tracked\n");
        std::free(released);
        std::free(retained);
        profiler.cancel(error);
        return false;
    }

    spark::AllocationSnapshot before_free;
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, before_free, error) || before_free.sample_count < 2) {
        std::fprintf(stderr, "retained live export: initial snapshot failed: %s (samples=%llu)\n", error.c_str(),
                     static_cast<unsigned long long>(before_free.sample_count));
        std::free(released);
        std::free(retained);
        profiler.cancel(error);
        return false;
    }
    std::free(released);
    if (!waitForCondition([&] { return profiler.freedAllocationSamples() != 0; }, 2s)) {
        std::fprintf(stderr, "retained live export: free was not tracked\n");
        std::free(retained);
        profiler.cancel(error);
        return false;
    }

    spark::AllocationSnapshot after_free;
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, after_free, error) ||
        after_free.sampled_bytes >= before_free.sampled_bytes) {
        std::fprintf(stderr, "retained live export: free was not reflected\n");
        std::free(retained);
        profiler.cancel(error);
        return false;
    }

    void *resized = std::realloc(retained, 2 * 1024 * 1024);
    if (resized == nullptr) {
        std::fprintf(stderr, "retained live export: realloc failed\n");
        std::free(retained);
        profiler.cancel(error);
        return false;
    }
    retained = resized;
    spark::AllocationSnapshot after_realloc;
    spark::AllocationSnapshot repeated;
    const std::string live_profile = profiler.liveExport({});
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, after_realloc, error) ||
        !spark::ProfilerTestAccess::allocationSnapshot(profiler, repeated, error) || live_profile.empty() ||
        live_profile.find("Allocation live-only") == std::string::npos ||
        after_realloc.sampled_bytes < after_free.sampled_bytes || repeated.sample_count != after_realloc.sample_count ||
        repeated.sampled_bytes != after_realloc.sampled_bytes ||
        !spark::ProfilerTestAccess::allocationSamplerRunning(profiler) ||
        !spark::ProfilerTestAccess::allocationHooksInstalled(profiler)) {
        std::fprintf(stderr, "retained live export: realloc or repeated snapshot was invalid\n");
        std::free(retained);
        profiler.cancel(error);
        return false;
    }

    if (!profiler.stopSampling(error)) {
        std::fprintf(stderr, "retained live export: stop failed: %s (lifecycle=%llu, contention=%llu)\n", error.c_str(),
                     static_cast<unsigned long long>(spark::ProfilerTestAccess::allocationLifecycleDropped(profiler)),
                     static_cast<unsigned long long>(spark::ProfilerTestAccess::allocationContentionDropped(profiler)));
        std::free(retained);
        return false;
    }
    const std::string final_profile = profiler.exportData({});
    const bool valid = !final_profile.empty() && profiler.sampleCount() == repeated.sample_count &&
                       profiler.sampledAllocationBytes() == repeated.sampled_bytes;
    std::free(retained);
    return profiler.shutdown(error) && valid;
}
#endif

}  // namespace

int main()
{
#if defined(_WIN32) || defined(__linux__)
    if (!verifyPersistentAllocationExportOrdering()) {
        return 1;
    }
#endif
#ifdef __linux__
    if (!verifyRetainedAllocationProfile() || !verifyAllocationLiveExport() || !verifyRetainedAllocationLiveExport()) {
        return 1;
    }
#endif
    return 0;
}