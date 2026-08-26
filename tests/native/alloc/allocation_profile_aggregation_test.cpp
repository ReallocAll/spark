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

Sample sample(std::uint64_t thread_id, std::int32_t window, std::uint64_t rva, std::uint64_t weight = 1)
{
    Sample result;
    result.thread_id = thread_id;
    result.os_thread_id = 1000 + thread_id;
    result.thread_name = "thread";
    result.window = window;
    result.weight = weight;
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
        assert(!aggregation.processSample(sample(1, window, tick)));
    }
    assert(!aggregation.processSample(sample(1, window, 999999)));
    assert(aggregation.pendingCapacityDrops() == 1);
    assert(aggregation.pendingSampleDrops() == 1);
    aggregation.finishPending();
    assert(aggregation.pendingFinalDrops() == AllocationProfileAggregation::kPendingSampleCapacity);
    assert(aggregation.pendingSampleDrops() == AllocationProfileAggregation::kPendingSampleCapacity + 1);
    assert(aggregation.droppedSamples() == AllocationProfileAggregation::kPendingSampleCapacity + 1);
}

}  // namespace
}  // namespace spark

int main()
{
    spark::transactionalCapacityAndReclamation();
    spark::cumulativeWindowBound();
    spark::liveWindowAndBoundedIdentities();
    spark::pendingDrops();
    return 0;
}
