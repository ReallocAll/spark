#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/profiler/profiler.h"
#include "native/alloc/allocation_sampler.h"
#include "native/sampler/recovery_sink.h"
#include "native/sampler/thread_info.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t KSlowEventDelayUs = 100000;
constexpr std::uint32_t KLiveRecordDelayUs = 20000;
constexpr std::uint32_t KAggregatorParkMs = 4000;
constexpr std::uint32_t KHealthyParkMs = 600;
constexpr std::uint32_t KStartupSettleMs = 200;
constexpr std::size_t KBacklogAllocations = 8000;
constexpr std::size_t KHealthyBacklogAllocations = 2000;
constexpr std::size_t KHealthyAllocationBytes = 128;
constexpr std::size_t KLiveRetainedAllocations = 2000;
constexpr std::uint64_t KDrainBudgetMs = 1500;
constexpr std::uint64_t KExitDeadlineMs = 2000;
constexpr std::uint64_t KStopWallClockBoundMs = KDrainBudgetMs + 400;
constexpr std::uint64_t KPathologicalStopBoundMs = KExitDeadlineMs + 400;
constexpr std::uint64_t KLiveFinalizeStopBoundMs = KDrainBudgetMs + 500;
constexpr std::uint64_t KDeadlineSlackMs = 1500;
constexpr std::uint64_t KQuiesceWaitMs = 500;
constexpr std::uint64_t KReapRetryBoundMs = KAggregatorParkMs * 3;
constexpr std::uint32_t KJournalParkMs = 7000;

int fail(const char *message)
{
    std::fprintf(stderr, "windows allocation stop: %s\n", message);
    return 1;
}

std::uint64_t elapsedMs(Clock::time_point started) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

void note(const char *shape, std::uint64_t first, std::uint64_t second, std::uint64_t third)
{
    std::fprintf(stderr, "info: %s a=%llu b=%llu c=%llu\n", shape, static_cast<unsigned long long>(first),
                 static_cast<unsigned long long>(second), static_cast<unsigned long long>(third));
}

class CountingRecoverySink : public spark::RecoverySink {
public:
    void journalModuleDef(std::uint32_t module_id, std::string_view path) override
    {
        (void)module_id;
        (void)path;
    }
    void journalThreadDef(std::uint64_t thread_id, std::uint64_t os_thread_id, std::string_view name) override
    {
        (void)thread_id;
        (void)os_thread_id;
        (void)name;
    }
    void journalSample(const spark::Sample &sample) override
    {
        (void)sample;
        samples_.fetch_add(1, std::memory_order_relaxed);
    }
    void journalTickEvent(std::uint64_t tick_id, double mspt) override
    {
        (void)tick_id;
        (void)mspt;
    }

    std::uint64_t samples() const noexcept { return samples_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> samples_{0};
};

spark::AllocationSamplerConfig makeConfig()
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = spark::currentNativeThreadId();
    config.all_threads = true;
    return config;
}

void allocationBurst(std::size_t count, std::size_t bytes)
{
    for (std::size_t i = 0; i < count; ++i) {
        void *pointer = std::malloc(bytes);
        if (pointer == nullptr) {
            continue;
        }
        static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
        std::free(pointer);
    }
}

// Reads the exported call trees the way the serializer does.
std::size_t readableTreeNodeCount(const spark::AllocationSampler &sampler)
{
    std::size_t nodes = sampler.tree().storageUsage().child_nodes;
    for (const auto &[thread_id, thread] : sampler.threadTrees()) {
        (void)thread_id;
        nodes += thread.tree.storageUsage().child_nodes;
    }
    return nodes;
}

bool report(const char *stage, const std::string &error, std::uint64_t stop_ms, std::uint64_t truncated)
{
    std::fprintf(stderr, "windows allocation stop: %s (stop-ms=%llu truncated=%llu error=%s)\n", stage,
                 static_cast<unsigned long long>(stop_ms), static_cast<unsigned long long>(truncated), error.c_str());
    return false;
}

bool verifyPathologicalStopExportsTruncatedProfile()
{
    CountingRecoverySink sink;
    spark::AllocationSampler sampler;
    sampler.setRecoverySink(&sink);

    spark::AllocationSamplerConfig config = makeConfig();
    config.aggregator_per_event_delay_us_for_testing = KSlowEventDelayUs;
    std::string error;
    if (!sampler.start(config, error)) {
        return report("slow session did not start", error, 0, 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(KStartupSettleMs));

    allocationBurst(KBacklogAllocations, KHealthyAllocationBytes);
    sampler.onTick(50.0);
    const Clock::time_point stop_started = Clock::now();
    const bool stopped = sampler.stop(error);
    const std::uint64_t stop_ms = elapsedMs(stop_started);
    const std::uint64_t truncated = sampler.drainTruncated();
    note("pathological", stop_ms, truncated, sampler.sampleCount());
    if (!stopped || !error.empty()) {
        return report("slow session did not stop", error, stop_ms, truncated);
    }
    if (stop_ms > KPathologicalStopBoundMs) {
        return report("stop exceeded the exit deadline plus slack", error, stop_ms, truncated);
    }
    if (sampler.stopWaitTimedOut() || truncated == 0 || !sampler.dataIncomplete()) {
        return report("drain did not truncate as expected", error, stop_ms, truncated);
    }
    if (sampler.sampleCount() == 0 || sampler.sampledBytes() == 0 || sampler.tree().empty()) {
        return report("truncated profile has no usable samples", error, stop_ms, truncated);
    }
    if (readableTreeNodeCount(sampler) == 0) {
        return report("truncated profile trees are not readable", error, stop_ms, truncated);
    }
    if (sampler.eventQueueHighWaterMark() > spark::AllocationSampler::eventQueueCapacity()) {
        return report("event queue exceeded its capacity", error, stop_ms, truncated);
    }
    if (sampler.peakLiveSamples() > spark::AllocationSampler::liveIndexCapacity()) {
        return report("live index exceeded its capacity", error, stop_ms, truncated);
    }
    if (sampler.droppedEvents() != 0) {
        return report("the event pool ran dry", error, stop_ms, truncated);
    }
    if (sampler.liveSamples() > sampler.peakLiveSamples()) {
        return report("live index accounting is inconsistent", error, stop_ms, truncated);
    }

    const std::uint64_t journalled = sink.samples();
    if (journalled == 0) {
        return report("no allocation sample was journalled", error, stop_ms, truncated);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(KQuiesceWaitMs));
    allocationBurst(KBacklogAllocations, KHealthyAllocationBytes);
    sampler.onTick(50.0);
    if (sink.samples() != journalled || sampler.droppedEvents() != 0) {
        return report("journal grew after the session stopped", error, stop_ms, truncated);
    }

    config.aggregator_per_event_delay_us_for_testing = 0;
    if (!sampler.start(config, error)) {
        return report("event pool did not survive truncation", error, stop_ms, truncated);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(KStartupSettleMs));
    allocationBurst(KHealthyBacklogAllocations, KHealthyAllocationBytes);
    std::this_thread::sleep_for(std::chrono::milliseconds(KQuiesceWaitMs));
    sampler.onTick(50.0);
    const Clock::time_point follow_up_started = Clock::now();
    const bool follow_up_stopped = sampler.stop(error);
    const std::uint64_t follow_up_ms = elapsedMs(follow_up_started);
    if (!follow_up_stopped || !error.empty()) {
        return report("follow-up session did not stop", error, follow_up_ms, sampler.drainTruncated());
    }
    if (follow_up_ms > KStopWallClockBoundMs) {
        return report("follow-up stop exceeded the drain budget", error, follow_up_ms, sampler.drainTruncated());
    }
    if (sampler.stopWaitTimedOut() || sampler.drainTruncated() != 0) {
        return report("follow-up session truncated its drain", error, follow_up_ms, sampler.drainTruncated());
    }
    if (sampler.droppedEvents() != 0 || sampler.sampleCount() == 0) {
        return report("follow-up session lost events", error, follow_up_ms, sampler.drainTruncated());
    }
    if (!sampler.shutdown(error) || sampler.running() || sampler.hooksInstalled()) {
        return report("shutdown after truncation failed", error, follow_up_ms, truncated);
    }
    return true;
}

bool verifyHealthyDrainKeepsProfileComplete()
{
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config = makeConfig();
    config.aggregator_delay_ms_for_testing = KHealthyParkMs;
    std::string error;
    if (!sampler.start(config, error)) {
        return report("healthy session did not start", error, 0, 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(KStartupSettleMs));

    allocationBurst(KHealthyBacklogAllocations, KHealthyAllocationBytes);
    sampler.onTick(50.0);
    const Clock::time_point stop_started = Clock::now();
    const bool stopped = sampler.stop(error);
    const std::uint64_t stop_ms = elapsedMs(stop_started);
    const std::uint64_t truncated = sampler.drainTruncated();
    note("healthy", stop_ms, truncated, sampler.sampleCount());
    if (!stopped || !error.empty()) {
        return report("healthy session did not stop", error, stop_ms, truncated);
    }
    if (stop_ms > KStopWallClockBoundMs) {
        return report("healthy stop exceeded the drain budget", error, stop_ms, truncated);
    }
    if (sampler.stopWaitTimedOut()) {
        return report("healthy stop waited past its deadline", error, stop_ms, truncated);
    }
    if (truncated != 0) {
        return report("healthy backlog was truncated", error, stop_ms, truncated);
    }
    if (sampler.droppedEvents() != 0) {
        return report("healthy event pool ran dry", error, stop_ms, truncated);
    }
    if (sampler.lifecycleDropped() != 0) {
        return report("healthy lifecycle records were dropped", error, stop_ms, truncated);
    }
    if (sampler.contentionDropped() != 0) {
        return report("healthy lifecycle hit lock contention", error, stop_ms, truncated);
    }
    if (sampler.droppedTickEvents() != 0) {
        return report("healthy tick events were dropped", error, stop_ms, truncated);
    }
    if (sampler.threadStateDrops() != 0) {
        return report("healthy thread states were dropped", error, stop_ms, truncated);
    }
    if (sampler.droppedSamples() != 0) {
        return report("healthy samples were dropped", error, stop_ms, truncated);
    }
    if (sampler.sampleCount() == 0 || sampler.tree().empty()) {
        return report("healthy profile tree is inconsistent", error, stop_ms, truncated);
    }
    if (readableTreeNodeCount(sampler) == 0) {
        return report("healthy profile trees are not readable", error, stop_ms, truncated);
    }
    if (sampler.dataIncomplete()) {
        return report("healthy profile was marked incomplete", error, stop_ms, truncated);
    }
    if (!sampler.shutdown(error) || sampler.running() || sampler.hooksInstalled()) {
        return report("healthy shutdown failed", error, stop_ms, truncated);
    }
    return true;
}

bool verifyLiveProfileFinalizationTruncates()
{
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config = makeConfig();
    config.live_only = true;
    config.live_finalize_per_record_delay_us_for_testing = KLiveRecordDelayUs;
    std::string error;
    if (!sampler.start(config, error)) {
        return report("live-only session did not start", error, 0, 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(KStartupSettleMs));

    std::vector<void *> retained;
    retained.reserve(KLiveRetainedAllocations);
    for (std::size_t i = 0; i < KLiveRetainedAllocations; ++i) {
        void *pointer = std::malloc(1024);
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
            retained.push_back(pointer);
        }
    }
    const std::uint64_t live_total = sampler.liveSamples();
    const Clock::time_point stop_started = Clock::now();
    const bool stopped = sampler.stop(error);
    const std::uint64_t stop_ms = elapsedMs(stop_started);
    const std::uint64_t truncated = sampler.drainTruncated();
    note("live-finalize", stop_ms, truncated, live_total);
    for (void *pointer : retained) {
        std::free(pointer);
    }
    if (!stopped || !error.empty()) {
        return report("live-only session did not stop", error, stop_ms, truncated);
    }
    if (stop_ms > KLiveFinalizeStopBoundMs) {
        return report("live finalization exceeded the drain budget plus slack", error, stop_ms, truncated);
    }
    if (truncated == 0 || truncated >= live_total) {
        return report("live finalization did not truncate the walk", error, stop_ms, truncated);
    }
    if (sampler.sampleCount() == 0 || !sampler.dataIncomplete()) {
        return report("walked live records were not kept in the profile", error, stop_ms, truncated);
    }
    if (sampler.sampleCount() + truncated > live_total) {
        return report("live finalization skipped-count is inconsistent", error, stop_ms, truncated);
    }
    if (readableTreeNodeCount(sampler) == 0) {
        return report("live profile trees are not readable", error, stop_ms, truncated);
    }
    if (!sampler.shutdown(error) || sampler.running() || sampler.hooksInstalled()) {
        return report("live-only shutdown failed", error, stop_ms, truncated);
    }
    return true;
}

bool verifyStopWaitTimeoutReapsTheAggregator()
{
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config = makeConfig();
    config.aggregator_delay_ms_for_testing = KAggregatorParkMs;
    std::string error;
    if (!sampler.start(config, error)) {
        return report("parked session did not start", error, 0, 0);
    }

    allocationBurst(KHealthyBacklogAllocations, KHealthyAllocationBytes);
    sampler.onTick(50.0);
    const Clock::time_point stop_started = Clock::now();
    const bool stopped = sampler.stop(error);
    const std::uint64_t stop_ms = elapsedMs(stop_started);
    note("fail-closed", stop_ms, sampler.drainTruncated(), sampler.sampleCount());
    if (stopped || error.empty()) {
        return report("timed-out stop did not fail closed", error, stop_ms, 0);
    }
    if (stop_ms < KExitDeadlineMs / 2 || stop_ms > KExitDeadlineMs + KDeadlineSlackMs) {
        return report("stop did not honor the exit deadline", error, stop_ms, 0);
    }
    if (error.find("aggregator failed") != std::string::npos) {
        return report("stop timeout was reported as an aggregator failure", error, stop_ms, 0);
    }
    std::string failure;
    if (!sampler.stopWaitTimedOut() || !sampler.dataIncomplete() || sampler.failure(failure)) {
        return report("timed-out stop was not reported honestly", error, stop_ms, 0);
    }
    if (!sampler.aggregatorMayBeAlive()) {
        return report("a timed-out stop abandoned its aggregator", error, stop_ms, 0);
    }
    spark::AllocationSnapshot snapshot;
    if (sampler.snapshot(snapshot, failure) || sampler.running()) {
        return report("a stopped session still offered export data", error, stop_ms, 0);
    }
    if (sampler.start(makeConfig(), error)) {
        return report("an un-reaped aggregator accepted a new session", error, stop_ms, 0);
    }

    const Clock::time_point reap_started = Clock::now();
    bool reaped = false;
    while (elapsedMs(reap_started) < KReapRetryBoundMs) {
        if (sampler.stop(error)) {
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!reaped) {
        return report("the abandoned aggregator was never reaped", error, stop_ms, 0);
    }
    if (sampler.aggregatorMayBeAlive()) {
        return report("a reaped aggregator stayed joinable", error, stop_ms, 0);
    }
    if (!sampler.shutdown(error) || sampler.running() || sampler.hooksInstalled()) {
        return report("shutdown after a reaped stop failed", error, stop_ms, 0);
    }
    return true;
}

std::filesystem::path uniqueJournalDir()
{
    static std::atomic<std::uint64_t> counter{0};
    return std::filesystem::temp_directory_path() / ("spark-alloc-journal-" + std::to_string(::GetCurrentProcessId()) +
                                                     "-" + std::to_string(counter.fetch_add(1)));
}

bool verifyRecoveryJournalOutlivesUnreapedAggregator()
{
    const std::filesystem::path journal_dir = uniqueJournalDir();
    spark::Profiler profiler;
    profiler.setRecoveryDirectory(journal_dir);
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 1;
    options.allocation_aggregator_delay_ms_for_testing = KJournalParkMs;
    std::string error;
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::filesystem::remove_all(journal_dir);
        return report("recovery session did not start", error, 0, 0);
    }
    if (!std::filesystem::is_directory(journal_dir)) {
        profiler.shutdown(error);
        return report("the recovery writer never created its journal", error, 0, 0);
    }

    allocationBurst(KHealthyBacklogAllocations, KHealthyAllocationBytes);
    profiler.onTick(50.0);
    const Clock::time_point cancel_started = Clock::now();
    const bool cancelled = profiler.cancel(error);
    const std::uint64_t cancel_ms = elapsedMs(cancel_started);
    note("journal gating", cancel_ms, cancelled ? 1U : 0U, 0);
    if (cancelled || error.empty()) {
        profiler.shutdown(error);
        std::filesystem::remove_all(journal_dir);
        return report("cancel reported success with an un-reaped aggregator", error, cancel_ms, 0);
    }
    if (!std::filesystem::is_directory(journal_dir)) {
        profiler.shutdown(error);
        return report("the journal was torn down while the aggregator was un-reaped", error, cancel_ms, 0);
    }

    profiler.discardRecoveryJournal();
    if (!std::filesystem::is_directory(journal_dir)) {
        profiler.shutdown(error);
        return report("discard removed the journal before the aggregator was reaped", error, cancel_ms, 0);
    }

    const Clock::time_point reap_started = Clock::now();
    bool reaped = false;
    while (elapsedMs(reap_started) < KJournalParkMs + KDeadlineSlackMs) {
        if (profiler.cancel(error)) {
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!reaped) {
        profiler.shutdown(error);
        std::filesystem::remove_all(journal_dir);
        return report("cancel never completed the teardown", error, cancel_ms, 0);
    }
    if (std::filesystem::is_directory(journal_dir)) {
        profiler.shutdown(error);
        return report("the journal was never discarded after reaping", error, cancel_ms, 0);
    }
    if (!profiler.shutdown(error)) {
        return report("shutdown after a gated cancel failed", error, cancel_ms, 0);
    }
    std::filesystem::remove_all(journal_dir);
    return true;
}

}  // namespace

int main()
{
    if (!verifyPathologicalStopExportsTruncatedProfile()) {
        return fail("truncated-export check failed");
    }
    if (!verifyHealthyDrainKeepsProfileComplete()) {
        return fail("healthy completeness check failed");
    }
    if (!verifyLiveProfileFinalizationTruncates()) {
        return fail("live finalization truncation check failed");
    }
    if (!verifyStopWaitTimeoutReapsTheAggregator()) {
        return fail("bounded stop-wait check failed");
    }
    if (!verifyRecoveryJournalOutlivesUnreapedAggregator()) {
        return fail("recovery journal gating check failed");
    }
    return 0;
}
