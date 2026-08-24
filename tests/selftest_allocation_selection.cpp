#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>

#include "core/profiler/profiler.h"
#include "native/alloc/allocation_thread_filter.h"
#include "native/sampler/thread_info.h"
#include "selftest_allocation_internal.h"

namespace spark::selftest {

bool verifyThreadSelectorSemantics()
{
    std::string error;
    spark::ThreadSelector selector;
    if (!selector.configure(false, false, {"alpha", "BETA"}, error) || !selector.matches("ALPHA") ||
        !selector.matches("beta") || selector.matches("alphabet")) {
        std::fprintf(stderr, "thread selector: exact-name semantics failed\n");
        return false;
    }
    if (!selector.configure(false, true, {R"(worker-\d+)", "server"}, error) || !selector.matches("WORKER-42") ||
        !selector.matches("Server") || selector.matches("worker-42-extra")) {
        std::fprintf(stderr, "thread selector: regex/full-match semantics failed\n");
        return false;
    }
    if (selector.configure(false, true, {"["}, error) || error.find("invalid thread name regex") == std::string::npos) {
        std::fprintf(stderr, "thread selector: invalid regex was accepted\n");
        return false;
    }
    if (!selector.configure(true, false, {}, error) || !selector.matches("anything")) {
        std::fprintf(stderr, "thread selector: all-thread semantics failed\n");
        return false;
    }

    spark::AllocationThreadFilter identities(256, 16);
    if (!identities.configure(false, false, {"spark-id-a"}, error)) {
        return false;
    }
    const std::uint64_t tid = spark::currentNativeThreadId();
    if (!setCurrentThreadName("spark-id-a")) {
        std::fprintf(stderr, "thread selector: could not name current thread\n");
        return false;
    }
    const spark::AllocationThreadSelection first = identities.resolve(1, tid);
    if (!setCurrentThreadName("spark-id-b")) {
        return false;
    }
    const spark::AllocationThreadSelection second = identities.resolve(2, tid);
    const spark::AllocationThreadSelection replay = identities.resolve(1, tid);
    if (!first.selected || second.selected || !replay.selected || replay.display_name != first.display_name) {
        std::fprintf(stderr, "thread selector: session identity/TID reuse isolation failed\n");
        return false;
    }

    spark::AllocationThreadFilter unavailable(256, 16);
    if (!unavailable.configure(false, false, {"Thread 18446744073709551615"}, error)) {
        return false;
    }
    const auto missing = unavailable.resolve(1, std::numeric_limits<std::uint64_t>::max());
    if (missing.selected || missing.name_available || unavailable.nameFailures() != 1) {
        std::fprintf(stderr, "thread selector: unavailable names did not fail closed\n");
        return false;
    }
    spark::AllocationThreadFilter bounded(256, 1);
    if (!bounded.configure(false, false, {"spark-id-b"}, error) || !bounded.resolve(1, tid).selected ||
        bounded.resolve(2, tid).selected || bounded.cacheDrops() != 1) {
        std::fprintf(stderr, "thread selector: identity cache did not fail bounded\n");
        return false;
    }
    return true;
}

#ifdef __linux__
bool verifyAllocationThreadSelection()
{
    std::string error;
    spark::AllocationSampler sampler;

    spark::AllocationSamplerConfig exact;
    exact.interval_bytes = 1;
    exact.session_seed = spark::currentNativeThreadId();
    exact.all_threads = false;
    exact.thread_patterns = {"SPARK-ALLOC-A", "spark-alloc-b"};
    if (!runNamedAllocationWorkers(sampler, exact, {"spark-alloc-a", "spark-alloc-b", "spark-alloc-x"}, error) ||
        sampler.sampleCount() == 0 || !allocationTreesHaveOnly(sampler, {"spark-alloc-a", "spark-alloc-b"}) ||
        sampler.filteredSamples() == 0) {
        std::fprintf(stderr,
                     "allocation thread selection: exact/multiple selection failed "
                     "(samples=%llu filtered=%llu error=%s)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()),
                     static_cast<unsigned long long>(sampler.filteredSamples()), error.c_str());
        return false;
    }

    spark::AllocationSamplerConfig regex = exact;
    regex.regex_threads = true;
    regex.thread_patterns = {R"(spark-dyn-\d+)"};
    if (!runNamedAllocationWorkers(sampler, regex, {"spark-dyn-42", "spark-other"}, error) ||
        !allocationTreesHaveOnly(sampler, {"spark-dyn-42"})) {
        std::fprintf(stderr, "allocation thread selection: regex/dynamic selection failed: %s\n", error.c_str());
        return false;
    }

    spark::AllocationSamplerConfig no_match = exact;
    no_match.thread_patterns = {"spark-never"};
    if (!runNamedAllocationWorkers(sampler, no_match, {"spark-no-match"}, error) || sampler.sampleCount() != 0 ||
        !sampler.threadTrees().empty() || sampler.filteredSamples() == 0) {
        std::fprintf(stderr,
                     "allocation thread selection: no-match profile was not empty "
                     "(samples=%llu roots=%zu filtered=%llu error=%s)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()), sampler.threadTrees().size(),
                     static_cast<unsigned long long>(sampler.filteredSamples()), error.c_str());
        return false;
    }

    spark::AllocationSamplerConfig invalid = exact;
    invalid.regex_threads = true;
    invalid.thread_patterns = {"["};
    if (sampler.start(invalid, error) || error.find("invalid thread name regex") == std::string::npos) {
        std::fprintf(stderr, "allocation thread selection: invalid regex was accepted\n");
        sampler.stop(error);
        return false;
    }

    spark::AllocationSamplerConfig retained = exact;

    retained.live_only = true;
    retained.thread_patterns = {"spark-live-src"};
    std::atomic<std::uint64_t> observed_identities{0};
    retained.observed_thread_identities_for_testing = &observed_identities;
    if (!sampler.start(retained, error)) {
        std::fprintf(stderr, "allocation thread selection: live-only start failed: %s\n", error.c_str());
        return false;
    }
    std::atomic<void *> retained_pointer{nullptr};
    std::atomic<void *> released_pointer{nullptr};
    std::atomic<bool> identity_observed{false};
    std::thread allocator([&] {
        setCurrentThreadName("spark-live-src");
        retained_pointer.store(std::malloc(8192), std::memory_order_release);
        released_pointer.store(std::malloc(4096), std::memory_order_release);
        identity_observed.store(waitForCondition(
            [&] { return observed_identities.load(std::memory_order_acquire) != 0; }, std::chrono::seconds(2)));
    });
    allocator.join();
    if (!identity_observed.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "allocation thread selection: aggregator did not observe the live allocator thread\n");
        sampler.stop(error);
        return false;
    }
    std::thread releaser([&] {
        setCurrentThreadName("spark-live-free");
        void *pointer = released_pointer.load(std::memory_order_acquire);
        void *replacement = std::realloc(pointer, 16384);
        std::free(replacement != nullptr ? replacement : pointer);
    });
    releaser.join();
    std::atomic<void *> unselected_retained{nullptr};
    std::thread unselected([&] {
        setCurrentThreadName("spark-live-no");
        unselected_retained.store(std::malloc(2048), std::memory_order_release);
    });
    unselected.join();
    sampler.onTick(50.0);
    const bool live_stopped = sampler.stop(error);
    const bool live_valid = live_stopped && sampler.sampleCount() != 0 && sampler.freedSamples() != 0 &&
                            allocationTreesHaveOnly(sampler, {"spark-live-src"});
    std::free(retained_pointer.load(std::memory_order_acquire));
    std::free(unselected_retained.load(std::memory_order_acquire));
    if (!live_valid) {
        std::fprintf(stderr,
                     "allocation thread selection: live-only/cross-thread lifecycle failed "
                     "(samples=%llu freed=%llu roots=%zu error=%s)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()),
                     static_cast<unsigned long long>(sampler.freedSamples()), sampler.threadTrees().size(),
                     error.c_str());
        return false;
    }
    if (!sampler.shutdown(error)) {
        return false;
    }

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 1;
    options.threads = {"spark-prof-a", "spark-prof-b"};
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "allocation thread selection: ProfilerOptions selector was rejected: %s\n", error.c_str());
        return false;
    }
    std::thread profiler_worker([] {
        setCurrentThreadName("spark-prof-b");
        allocationBurst();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    });
    profiler_worker.join();
    profiler.onTick(50.0);
    if (!profiler.stopSampling(error) || profiler.sampleCount() == 0) {
        std::fprintf(stderr,
                     "allocation thread selection: profiler integration failed "
                     "(samples=%llu error=%s)\n",
                     static_cast<unsigned long long>(profiler.sampleCount()), error.c_str());
        return false;
    }

    options.threads = {"*"};
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "allocation thread selection: * selector start failed: %s\n", error.c_str());
        return false;
    }
    allocationBurst();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    profiler.onTick(50.0);
    if (!profiler.stopSampling(error) || profiler.sampleCount() == 0) {
        std::fprintf(stderr, "allocation thread selection: * selector captured no samples\n");
        return false;
    }

    options.threads = {"*", "spark-prof-b"};
    if (profiler.start(options, spark::currentNativeThreadId(), error) ||
        error.find("--thread * cannot be combined") == std::string::npos) {
        std::fprintf(stderr, "allocation thread selection: ambiguous * selector was accepted\n");
        profiler.cancel();
        return false;
    }
    options.threads.clear();
    options.regex = true;
    if (profiler.start(options, spark::currentNativeThreadId(), error) ||
        error.find("--regex requires") == std::string::npos) {
        std::fprintf(stderr, "allocation thread selection: patternless regex was accepted\n");
        profiler.cancel();
        return false;
    }
    return profiler.shutdown(error);
}
#endif

}  // namespace spark::selftest
