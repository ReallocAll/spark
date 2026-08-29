#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "native/alloc/allocation_sampler.h"
#include "native/sampler/thread_info.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view KRetainedProfileDiscardedError =
    "allocation lifecycle tracking was incomplete; retained profile discarded";

struct AllocationDiagnostics {
    const char *stop_outcome = "not_profiled";
    std::uint64_t sample_count = 0;
    std::uint64_t dropped_all_trials = 0;
    std::uint64_t observed_bytes = 0;
    std::uint64_t freed_samples = 0;
    std::uint64_t live_samples = 0;
    std::uint64_t peak_live_samples = 0;
    std::uint64_t lifecycle_dropped = 0;
    std::uint64_t contention_dropped = 0;
    std::uint64_t dropped_samples = 0;
    std::uint64_t dropped_events = 0;
    std::uint64_t dropped_tick_events = 0;
    std::uint64_t thread_state_drops = 0;
    std::uint64_t thread_identity_cache_drops = 0;
    std::uint64_t profile_storage_sample_drops = 0;
    std::uint64_t pending_sample_drops = 0;
    bool profile_storage_exhausted = false;
    bool data_incomplete = false;
};

AllocationDiagnostics collectDiagnostics(const spark::AllocationSampler &sampler, const char *stop_outcome)
{
    AllocationDiagnostics diagnostics;
    diagnostics.stop_outcome = stop_outcome;
    diagnostics.sample_count = sampler.sampleCount();
#ifdef SPARK_ALLOCATION_BENCHMARK_CURRENT
    diagnostics.dropped_all_trials = sampler.droppedEvents();
#else
    diagnostics.dropped_all_trials = sampler.droppedSamples();
#endif
    diagnostics.observed_bytes = sampler.observedBytes();
    diagnostics.freed_samples = sampler.freedSamples();
    diagnostics.live_samples = sampler.liveSamples();
    diagnostics.peak_live_samples = sampler.peakLiveSamples();
    diagnostics.lifecycle_dropped = sampler.lifecycleDropped();
    diagnostics.contention_dropped = sampler.contentionDropped();
    diagnostics.dropped_samples = sampler.droppedSamples();
    diagnostics.dropped_events = sampler.droppedEvents();
    diagnostics.dropped_tick_events = sampler.droppedTickEvents();
    diagnostics.thread_state_drops = sampler.threadStateDrops();
    diagnostics.thread_identity_cache_drops = sampler.threadIdentityCacheDrops();
    diagnostics.profile_storage_sample_drops = sampler.profileStorageSampleDrops();
    diagnostics.pending_sample_drops = sampler.pendingSampleDrops();
    diagnostics.profile_storage_exhausted = sampler.profileStorageExhausted();
    diagnostics.data_incomplete = sampler.dataIncomplete();
    return diagnostics;
}

void allocationWork(std::size_t operations)
{
    for (std::size_t i = 0; i < operations; ++i) {
        void *pointer = std::malloc(64 + (i & 63));
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
            std::free(pointer);
        }
    }
}

double measure(std::size_t threads, std::size_t operations_per_thread)
{
    const auto start = Clock::now();
    if (threads == 1) {
        allocationWork(operations_per_thread);
    }
    else {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back(allocationWork, operations_per_thread);
        }
        for (std::thread &worker : workers) {
            worker.join();
        }
    }
    const auto elapsed = Clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count();
}

double runTrials(std::size_t threads, std::size_t operations_per_thread)
{
    std::vector<double> trials;
    trials.reserve(5);
    for (int trial = 0; trial < 5; ++trial) {
        trials.push_back(measure(threads, operations_per_thread));
    }
    std::ranges::sort(trials);
    return trials[trials.size() / 2];
}

void printResult(const char *name, std::size_t threads, std::int32_t interval, bool live_only, bool count_only,
                 std::size_t operations_per_thread, double elapsed_ns, const AllocationDiagnostics &diagnostics)
{
    const auto operations = static_cast<double>(threads * operations_per_thread);
    std::printf("%s,%zu,%d,%d,%d,%zu,%.0f,%.2f,%llu,%llu,%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%"
                "llu,%llu,%d,%llu,%d\n",
                name, threads, interval, live_only ? 1 : 0, count_only ? 1 : 0, threads * operations_per_thread,
                elapsed_ns, elapsed_ns / operations, static_cast<unsigned long long>(diagnostics.sample_count),
                static_cast<unsigned long long>(diagnostics.dropped_all_trials),
                static_cast<unsigned long long>(diagnostics.observed_bytes), diagnostics.stop_outcome,
                static_cast<unsigned long long>(diagnostics.freed_samples),
                static_cast<unsigned long long>(diagnostics.live_samples),
                static_cast<unsigned long long>(diagnostics.peak_live_samples),
                static_cast<unsigned long long>(spark::AllocationSampler::liveIndexCapacity()),
                static_cast<unsigned long long>(diagnostics.lifecycle_dropped),
                static_cast<unsigned long long>(diagnostics.contention_dropped),
                static_cast<unsigned long long>(diagnostics.dropped_samples),
                static_cast<unsigned long long>(diagnostics.dropped_events),
                static_cast<unsigned long long>(diagnostics.dropped_tick_events),
                static_cast<unsigned long long>(diagnostics.thread_state_drops),
                static_cast<unsigned long long>(diagnostics.thread_identity_cache_drops),
                static_cast<unsigned long long>(diagnostics.profile_storage_sample_drops),
                diagnostics.profile_storage_exhausted ? 1 : 0,
                static_cast<unsigned long long>(diagnostics.pending_sample_drops), diagnostics.data_incomplete ? 1 : 0);
}

void printDiagnostics(const char *name, std::size_t threads, bool stopped, const std::string &error,
                      const AllocationDiagnostics &diagnostics)
{
    std::fprintf(
        stderr,
        "diag,%s,%zu,stopped=%d,error=%s,freed=%llu,live=%llu,peak=%llu,live_index_capacity=%llu,"
        "lifecycle_dropped=%llu,contention_dropped=%llu,dropped_samples=%llu,dropped_events=%llu,"
        "dropped_tick_events=%llu,thread_state_drops=%llu,thread_identity_cache_drops=%llu,"
        "profile_storage_sample_drops=%llu,profile_storage_exhausted=%d,pending_sample_drops=%llu,"
        "data_incomplete=%d\n",
        name, threads, stopped ? 1 : 0, error.c_str(), static_cast<unsigned long long>(diagnostics.freed_samples),
        static_cast<unsigned long long>(diagnostics.live_samples),
        static_cast<unsigned long long>(diagnostics.peak_live_samples),
        static_cast<unsigned long long>(spark::AllocationSampler::liveIndexCapacity()),
        static_cast<unsigned long long>(diagnostics.lifecycle_dropped),
        static_cast<unsigned long long>(diagnostics.contention_dropped),
        static_cast<unsigned long long>(diagnostics.dropped_samples),
        static_cast<unsigned long long>(diagnostics.dropped_events),
        static_cast<unsigned long long>(diagnostics.dropped_tick_events),
        static_cast<unsigned long long>(diagnostics.thread_state_drops),
        static_cast<unsigned long long>(diagnostics.thread_identity_cache_drops),
        static_cast<unsigned long long>(diagnostics.profile_storage_sample_drops),
        diagnostics.profile_storage_exhausted ? 1 : 0,
        static_cast<unsigned long long>(diagnostics.pending_sample_drops), diagnostics.data_incomplete ? 1 : 0);
}

bool cleanSuccess(const AllocationDiagnostics &diagnostics)
{
    return std::string_view(diagnostics.stop_outcome) == "success" && diagnostics.lifecycle_dropped == 0 &&
           diagnostics.contention_dropped == 0;
}

bool expectedLiveContention(const AllocationDiagnostics &diagnostics, const std::string &error)
{
    return std::string_view(diagnostics.stop_outcome) == "stop_failed" &&
           std::string_view(error) == KRetainedProfileDiscardedError && diagnostics.lifecycle_dropped != 0 &&
           diagnostics.lifecycle_dropped == diagnostics.contention_dropped &&
           diagnostics.live_samples <= diagnostics.lifecycle_dropped &&
           diagnostics.peak_live_samples < spark::AllocationSampler::liveIndexCapacity() &&
           diagnostics.dropped_samples <= diagnostics.lifecycle_dropped && diagnostics.dropped_events == 0 &&
           diagnostics.dropped_tick_events == 0 && diagnostics.thread_state_drops == 0 &&
           diagnostics.thread_identity_cache_drops == 0 && diagnostics.profile_storage_sample_drops == 0 &&
           !diagnostics.profile_storage_exhausted && diagnostics.pending_sample_drops == 0;
}

bool expectedSaturated(const AllocationDiagnostics &diagnostics)
{
    return std::string_view(diagnostics.stop_outcome) == "success" && diagnostics.dropped_events != 0 &&
           diagnostics.lifecycle_dropped == diagnostics.contention_dropped &&
           diagnostics.dropped_events <= diagnostics.dropped_samples &&
           diagnostics.dropped_samples <= diagnostics.dropped_events + diagnostics.lifecycle_dropped &&
           diagnostics.live_samples <= diagnostics.lifecycle_dropped &&
           diagnostics.peak_live_samples < spark::AllocationSampler::liveIndexCapacity() &&
           diagnostics.dropped_tick_events == 0 && diagnostics.thread_state_drops == 0 &&
           diagnostics.thread_identity_cache_drops == 0 && diagnostics.profile_storage_sample_drops == 0 &&
           !diagnostics.profile_storage_exhausted && diagnostics.pending_sample_drops == 0 &&
           diagnostics.data_incomplete;
}

bool expectedNormal4kContention(const AllocationDiagnostics &diagnostics)
{
    return std::string_view(diagnostics.stop_outcome) == "success" &&
           diagnostics.lifecycle_dropped == diagnostics.contention_dropped &&
           diagnostics.dropped_samples <= diagnostics.lifecycle_dropped &&
           diagnostics.live_samples <= diagnostics.lifecycle_dropped &&
           diagnostics.peak_live_samples < spark::AllocationSampler::liveIndexCapacity() &&
           diagnostics.dropped_events == 0 && diagnostics.dropped_tick_events == 0 &&
           diagnostics.thread_state_drops == 0 && diagnostics.thread_identity_cache_drops == 0 &&
           diagnostics.profile_storage_sample_drops == 0 && !diagnostics.profile_storage_exhausted &&
           diagnostics.pending_sample_drops == 0 && diagnostics.data_incomplete == (diagnostics.lifecycle_dropped != 0);
}

bool validateCase(const char *name, std::size_t threads, const AllocationDiagnostics &diagnostics,
                  const std::string &error)
{
    const bool is_live = std::string_view(name) == "live-4k";
    const bool clean = cleanSuccess(diagnostics) && (!is_live || diagnostics.live_samples == 0);
    if (std::string_view(name) == "normal-4k" && threads == 4) {
        return clean || expectedNormal4kContention(diagnostics);
    }
    if (std::string_view(name) == "saturated" && diagnostics.dropped_events != 0) {
        return expectedSaturated(diagnostics);
    }
    return clean || (is_live && threads == 4 && expectedLiveContention(diagnostics, error));
}

bool runProfiledCase(spark::AllocationSampler &sampler, const char *name, std::size_t threads, std::int32_t interval,
                     bool live_only, bool count_only, std::size_t operations_per_thread, bool saturated)
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = interval;
    config.session_seed = spark::currentNativeThreadId();
    config.live_only = live_only;
    config.count_only = count_only;
#ifdef SPARK_ALLOCATION_BENCHMARK_CURRENT
    config.aggregator_delay_ms_for_testing = saturated ? 1000 : 0;
#else
    (void)saturated;
#endif

    std::string error;
    if (!sampler.start(config, error)) {
        const AllocationDiagnostics diagnostics = collectDiagnostics(sampler, "start_failed");
        printResult(name, threads, interval, live_only, count_only, operations_per_thread, 0.0, diagnostics);
        std::fprintf(stderr, "%s: start failed: %s\n", name, error.c_str());
        return false;
    }
    const double elapsed = runTrials(threads, operations_per_thread);
    const bool stopped = sampler.stop(error);
    const char *stop_outcome = stopped ? "success" : "stop_failed";
    AllocationDiagnostics diagnostics = collectDiagnostics(sampler, stop_outcome);
    printDiagnostics(name, threads, stopped, error, diagnostics);
    const bool accepted = validateCase(name, threads, diagnostics, error);
    if (!accepted) {
        std::fprintf(stderr, "%s: allocation benchmark oracle rejected case\n", name);
    }
    if (!stopped && error == KRetainedProfileDiscardedError) {
        diagnostics.stop_outcome = "retained_profile_discarded";
    }
    printResult(name, threads, interval, live_only, count_only, operations_per_thread, elapsed, diagnostics);
    return accepted;
}

}  // namespace

int main()
{
    constexpr std::size_t k_operations = 200000;
    constexpr std::size_t k_pressure_operations = 16384;

    std::printf("case,threads,interval,live_only,count_only,operations_per_trial,median_ns,"
                "ns_per_op,samples_all_trials,dropped_all_trials,observed_bytes,stop_outcome,freed_samples,"
                "live_samples,peak_live_samples,live_index_capacity,lifecycle_dropped,contention_dropped,"
                "dropped_samples,dropped_events,dropped_tick_events,thread_state_drops,thread_identity_cache_drops,"
                "profile_storage_sample_drops,profile_storage_exhausted,pending_sample_drops,data_incomplete\n");
    const AllocationDiagnostics not_profiled;
    printResult("unprofiled", 1, 0, false, false, k_operations, runTrials(1, k_operations), not_profiled);
    printResult("unprofiled", 4, 0, false, false, k_operations, runTrials(4, k_operations), not_profiled);

    spark::AllocationSampler sampler;
    bool all_cases_accepted = true;
    const auto run_case = [&](const char *name, std::size_t threads, std::int32_t interval, bool live_only,
                              bool count_only, std::size_t operations_per_thread, bool saturated) {
        const bool accepted =
            runProfiledCase(sampler, name, threads, interval, live_only, count_only, operations_per_thread, saturated);
        all_cases_accepted = accepted && all_cases_accepted;
    };
    run_case("count-only", 1, spark::kDefaultAllocationIntervalBytes, false, true, k_operations, false);
    run_case("count-only", 4, spark::kDefaultAllocationIntervalBytes, false, true, k_operations, false);
    printResult("disabled-hooks", 1, 0, false, false, k_operations, runTrials(1, k_operations), not_profiled);
    printResult("disabled-hooks", 4, 0, false, false, k_operations, runTrials(4, k_operations), not_profiled);

    run_case("normal-default", 1, spark::kDefaultAllocationIntervalBytes, false, false, k_operations, false);
    run_case("normal-default", 4, spark::kDefaultAllocationIntervalBytes, false, false, k_operations, false);
    run_case("normal-4k", 1, 4096, false, false, k_operations, false);
    run_case("normal-4k", 4, 4096, false, false, k_operations, false);
    run_case("live-4k", 1, 4096, true, false, k_operations, false);
    run_case("live-4k", 4, 4096, true, false, k_operations, false);
    run_case("saturated", 4, 1, false, false, k_pressure_operations, true);

    std::string error;
    if (!sampler.shutdown(error)) {
        std::fprintf(stderr, "shutdown failed: %s\n", error.c_str());
        all_cases_accepted = false;
    }
    return all_cases_accepted ? 0 : 1;
}
