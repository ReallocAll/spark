#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "native/alloc/allocation_profile_aggregation.h"
#include "profiling_window.h"

namespace spark {
namespace {

class RecordingSink final : public RecoverySink {
public:
    void journalModuleDef(std::uint32_t, std::string_view) override { ++modules; }
    void journalThreadDef(std::uint64_t, std::uint64_t os_thread_id, std::string_view) override
    {
        last_os_thread_id = os_thread_id;
        ++threads;
    }
    void journalSample(const Sample &) override { ++samples; }
    void journalTickEvent(std::uint64_t, double) override { ++ticks; }

    std::uint64_t modules = 0;
    std::uint64_t threads = 0;
    std::uint64_t samples = 0;
    std::uint64_t ticks = 0;
    std::uint64_t last_os_thread_id = 0;
};

AllocationSamplerConfig config(bool live_only = false)
{
    AllocationSamplerConfig result;
    result.session_seed = 1;
    result.live_only = live_only;
    return result;
}

Sample sample(std::uint64_t thread_id, std::int32_t window, std::uint64_t rva, std::uint64_t weight = 1,
              std::uint64_t tick_id = 0)
{
    Sample result;
    result.thread_id = thread_id;
    result.os_thread_id = 1000 + thread_id;
    result.thread_name = "thread";
    result.window = window;
    result.weight = weight;
    result.tick_id = tick_id;
    result.frames.push_back(FrameKey{.module = 1, .rva = rva, .raw_address = rva});
    return result;
}

void configure(AllocationProfileAggregation &aggregation, const AllocationSamplerConfig &cfg,
               RecoverySink *sink = nullptr)
{
    aggregation.reset(cfg, sink);
    std::string error;
    assert(aggregation.configure(error));
}

void transactionalCapacityAndReclamation()
{
    AllocationProfileAggregation aggregation;
    configure(aggregation, config());
    const std::int32_t now = profiling_window::windowNow();
    for (std::uint64_t i = 0; i < AllocationProfileAggregation::kProfileNodeCapacity / 2; ++i) {
        assert(aggregation.acceptSample(sample(1, now, i)));
    }
    const std::uint64_t accepted = aggregation.sampleCount();
    assert(aggregation.profileNodesRemaining() == 0);
    assert(aggregation.profileStorageExhausted());
    assert(!aggregation.acceptSample(sample(1, now, 1000000)));
    assert(aggregation.sampleCount() == accepted);
    assert(aggregation.droppedProfileSamples() == 1);

    AllocationProfileAggregation reclaimed;
    configure(reclaimed, config());
    assert(reclaimed.acceptSample(sample(1, now - profiling_window::kHistorySize - 1, 42, 7)));
    assert(reclaimed.acceptSample(sample(1, now, 43, 11)));
    assert(reclaimed.sampleCount() == 1);
    assert(reclaimed.sampledBytes() == 11);
    AllocationSnapshot snapshot;
    std::string error;
    assert(reclaimed.copyCumulativeSnapshot(snapshot, 0, error));
    assert(reclaimed.sampleCount() == 1);
    assert(reclaimed.sampledBytes() == 11);
    assert(reclaimed.historySamplesPruned() == 1);
    assert(reclaimed.historyBytesPruned() == 7);
    assert(reclaimed.profileNodesRemaining() == AllocationProfileAggregation::kProfileNodeCapacity - 2);
}

void cumulativeWindowBound()
{
    AllocationProfileAggregation aggregation;
    configure(aggregation, config());
    const std::int32_t now = profiling_window::windowNow();
    for (std::int32_t window = now - profiling_window::kHistorySize; window <= now; ++window) {
        assert(aggregation.acceptSample(sample(1, window, static_cast<std::uint64_t>(window), 3)));
    }
    AllocationSnapshot snapshot;
    std::string error;
    assert(aggregation.copyCumulativeSnapshot(snapshot, 0, error));
    assert(aggregation.retainedHistoryWindows() == profiling_window::kHistorySize + 1);
    assert(aggregation.sampleCount() == profiling_window::kHistorySize + 1);
    assert(aggregation.sampledBytes() == static_cast<std::uint64_t>(profiling_window::kHistorySize + 1) * 3);
}

void liveWindowAndBoundedIdentities()
{
    RecordingSink sink;
    AllocationProfileAggregation aggregation;
    configure(aggregation, config(true), &sink);
    const std::int32_t now = profiling_window::windowNow();
    std::vector<AllocationProfileAggregation::RetainedSample> retained;
    retained.push_back({.sample = sample(1, now - 1000, 1, 5), .age_ms = 10});
    retained.push_back({.sample = sample(257, now + 1000, 2, 7), .age_ms = 20});
    AllocationSnapshot snapshot;
    std::string error;
    assert(aggregation.buildLiveSnapshot(retained, snapshot, 0, error));
    assert(snapshot.sample_count == 2);
    assert(snapshot.sampled_bytes == 12);
    assert(snapshot.thread_trees.size() == 2);
    assert(snapshot.tree.root().times.size() == 1);
    aggregation.recordTick(now - 1000, 10.0);
    aggregation.recordTick(now + 1000, 20.0);
    assert(aggregation.windowTicks().size() == 1);
    assert(aggregation.windowTicks().begin()->first == snapshot.tree.root().times.begin()->first);
    assert(aggregation.windowTicks().begin()->second.ticks == 2);
    assert(aggregation.acceptLiveSample(sample(257, now, 3)));
    assert(sink.last_os_thread_id == 1257);

    for (std::uint64_t i = 0; i < 600; ++i) {
        (void)aggregation.internFrame("module-" + std::to_string(i), i, i);
    }
    assert(aggregation.modules().size() == AllocationProfileAggregation::kModuleCapacity);
    assert(aggregation.moduleOverflowFrames() > 0);
}

void pendingDrops()
{
    AllocationSamplerConfig cfg = config();
    cfg.only_ticks_over_ms = 1;
    AllocationProfileAggregation aggregation;
    configure(aggregation, cfg);
    const std::int32_t window = profiling_window::windowNow();
    for (std::uint64_t tick = 0; tick < AllocationProfileAggregation::kPendingSampleCapacity; ++tick) {
        assert(!aggregation.processSample(sample(1, window, tick, 1, 0)));
    }
    assert(!aggregation.processSample(sample(1, window, 999999, 1, 1)));
    assert(aggregation.pendingCapacityDrops() == 1);
    assert(aggregation.pendingSampleDrops() == 1);
    aggregation.finishPending(2);
    assert(aggregation.terminalInFlightTickSamplesDiscarded() == 0);
    assert(aggregation.pendingFinalDrops() == 0);
    assert(aggregation.pendingSampleDrops() == AllocationProfileAggregation::kPendingSampleCapacity + 1);
    assert(aggregation.droppedSamples() == AllocationProfileAggregation::kPendingSampleCapacity + 1);

    AllocationProfileAggregation legacy;
    configure(legacy, cfg);
    assert(!legacy.processSample(sample(1, window, 1, 1, 0)));
    legacy.finishPending();
    assert(legacy.terminalInFlightTickSamplesDiscarded() == 0);
    assert(legacy.pendingFinalDrops() == 0);
    assert(legacy.pendingSampleDrops() == 1);
    assert(legacy.droppedSamples() == 1);
}

void terminalTickClassification()
{
    AllocationSamplerConfig cfg = config();
    cfg.only_ticks_over_ms = 10;
    const std::int32_t window = profiling_window::windowNow();

    AllocationProfileAggregation current;
    configure(current, cfg);
    for (std::uint64_t rva = 1; rva <= 4; ++rva) {
        assert(!current.processSample(sample(1, window, rva, 1, 7)));
    }
    current.finishPending(7);
    assert(current.terminalInFlightTickSamplesDiscarded() == 4);
    assert(current.pendingFinalDrops() == 4);
    assert(current.pendingSampleDrops() == 0);
    assert(current.droppedSamples() == 0);
    assert(!current.dataIncomplete());
    assert(current.tree().root().times.empty());
    current.finishPending(7);
    assert(current.terminalInFlightTickSamplesDiscarded() == 4);
    assert(current.pendingFinalDrops() == 4);
    assert(current.pendingSampleDrops() == 0);
    assert(current.droppedSamples() == 0);

    configure(current, cfg);
    assert(current.terminalInFlightTickSamplesDiscarded() == 0);
    assert(current.pendingFinalDrops() == 0);
    assert(current.pendingSampleDrops() == 0);
    assert(current.droppedSamples() == 0);
    assert(!current.dataIncomplete());
    assert(!current.processSample(sample(1, window, 8, 1, 9)));
    current.finishPending(9);
    assert(current.terminalInFlightTickSamplesDiscarded() == 1);
    assert(current.pendingFinalDrops() == 1);
    assert(current.pendingSampleDrops() == 0);
    assert(current.droppedSamples() == 0);
    assert(!current.dataIncomplete());

    AllocationProfileAggregation mixed;
    configure(mixed, cfg);
    assert(!mixed.processSample(sample(1, window, 2, 1, 6)));
    assert(!mixed.processSample(sample(1, window, 3, 1, 7)));
    assert(!mixed.processSample(sample(1, window, 4, 1, 8)));
    mixed.finishPending(7);
    assert(mixed.terminalInFlightTickSamplesDiscarded() == 1);
    assert(mixed.pendingFinalDrops() == 1);
    assert(mixed.pendingSampleDrops() == 2);
    assert(mixed.droppedSamples() == 2);
    assert(mixed.dataIncomplete());
    assert(mixed.tree().root().times.empty());

    AllocationProfileAggregation completed;
    configure(completed, cfg);
    completed.processTick(0, 10.0);
    completed.processTick(1, 10.1);
    assert(!completed.processSample(sample(1, window, 5, 1, 0)));
    assert(completed.processSample(sample(1, window, 6, 1, 1)));
    completed.finishPending(2);
    assert(completed.terminalInFlightTickSamplesDiscarded() == 0);
    assert(completed.pendingSampleDrops() == 0);
    assert(completed.droppedSamples() == 0);
    assert(!completed.dataIncomplete());
    assert(completed.sampleCount() == 1);
    assert(completed.tree().root().times.size() == 1);

    AllocationProfileAggregation long_running;
    configure(long_running, cfg);
    constexpr std::uint64_t terminal_tick = AllocationProfileAggregation::kMaxTickDecisions + 1;
    assert(!long_running.processSample(sample(1, window, 7, 1, terminal_tick)));
    long_running.finishPending(terminal_tick);
    assert(long_running.terminalInFlightTickSamplesDiscarded() == 1);
    assert(long_running.pendingSampleDrops() == 0);
    assert(long_running.droppedSamples() == 0);
    assert(!long_running.dataIncomplete());

    AllocationProfileAggregation high_completed;
    configure(high_completed, cfg);
    constexpr std::uint64_t high_tick = AllocationProfileAggregation::kMaxTickDecisions + 1;
    high_completed.processTick(high_tick, 10.1);
    assert(high_completed.tickAccepts(high_tick));
    assert(high_completed.processSample(sample(1, window, 8, 1, high_tick)));
    assert(high_completed.sampleCount() == 1);
    assert(high_completed.pendingSampleDrops() == 0);
    assert(high_completed.droppedSamples() == 0);
    assert(!high_completed.dataIncomplete());

    AllocationProfileAggregation high_reordered;
    configure(high_reordered, cfg);
    assert(!high_reordered.processSample(sample(1, window, 11, 1, high_tick)));
    high_reordered.processTick(high_tick, 10.1);
    assert(high_reordered.sampleCount() == 1);
    assert(high_reordered.pendingSampleDrops() == 0);
    assert(high_reordered.droppedSamples() == 0);
    assert(!high_reordered.dataIncomplete());

    AllocationProfileAggregation high_filtered;
    configure(high_filtered, cfg);
    high_filtered.processTick(high_tick, 10.0);
    assert(!high_filtered.tickAccepts(high_tick));
    assert(!high_filtered.processSample(sample(1, window, 9, 1, high_tick)));
    assert(high_filtered.sampleCount() == 0);
    assert(high_filtered.pendingSampleDrops() == 0);
    assert(high_filtered.droppedSamples() == 0);
    assert(!high_filtered.dataIncomplete());

    AllocationProfileAggregation invariant;
    configure(invariant, cfg);
    constexpr std::uint64_t future_tick = high_tick + 1;
    assert(!invariant.processSample(sample(1, window, 10, 1, future_tick)));
    invariant.finishPending(high_tick);
    assert(invariant.terminalInFlightTickSamplesDiscarded() == 0);
    assert(invariant.pendingStaleDrops() == 1);
    assert(invariant.pendingSampleDrops() == 1);
    assert(invariant.droppedSamples() == 1);
    assert(invariant.dataIncomplete());
}

}  // namespace
}  // namespace spark

int main()
{
    spark::transactionalCapacityAndReclamation();
    spark::cumulativeWindowBound();
    spark::liveWindowAndBoundedIdentities();
    spark::pendingDrops();
    spark::terminalTickClassification();
    return 0;
}
