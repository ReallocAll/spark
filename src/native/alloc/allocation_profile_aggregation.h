#ifndef ENDSTONE_SPARK_ALLOCATION_PROFILE_AGGREGATION_H
#define ENDSTONE_SPARK_ALLOCATION_PROFILE_AGGREGATION_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native/alloc/allocation_sampler.h"
#include "native/alloc/allocation_thread_filter.h"

namespace spark {

class AllocationProfileAggregation {
public:
    static constexpr std::size_t kModuleCapacity = 512;
    static constexpr std::size_t kNamedThreadRootCapacity = 256;
    static constexpr std::size_t kThreadRootCapacity = kNamedThreadRootCapacity + 1;
    static constexpr std::size_t kProfileNodeCapacity = 131072;
    static constexpr std::size_t kProfileTimeEntryCapacity = 2 * 1024 * 1024;
    static constexpr std::size_t kPendingSampleCapacity = 32768;
    static constexpr std::size_t kMaxTickDecisions = 100000;

    struct RetainedSample {
        Sample sample;
        std::uint64_t age_ms = 0;
    };

    AllocationProfileAggregation();

    void reset(const AllocationSamplerConfig &config, RecoverySink *recovery_sink);
    bool configure(std::string &error);
    void setRecoverySink(RecoverySink *recovery_sink) noexcept { recovery_sink_ = recovery_sink; }

    AllocationThreadSelection resolveThread(std::uint64_t session_thread_id, std::uint64_t os_thread_id);
    void observeThread(std::uint64_t session_thread_id, std::uint64_t os_thread_id);
    FrameKey internFrame(std::string_view path, std::uint64_t rva, std::uint64_t raw_address);

    // Applies the tick filter and either admits the sample or places it in the
    // bounded pending set. Returns true only when the sample is admitted now.
    bool processSample(Sample sample);
    bool acceptSample(Sample sample);
    bool acceptLiveSample(Sample sample);
    bool tickAccepts(std::uint64_t tick_id) const noexcept;
    void processTick(std::uint64_t tick_id, double mspt_ms);
    void recordTick(std::int32_t window, double mspt_ms);
    void finishPending();

    bool copyCumulativeSnapshot(AllocationSnapshot &snapshot, std::uint64_t number_of_ticks, std::string &error);
    bool buildLiveSnapshot(const std::vector<RetainedSample> &retained, AllocationSnapshot &snapshot,
                           std::uint64_t number_of_ticks, std::string &error) const;

    const CallTree &tree() const noexcept { return tree_; }
    const std::map<std::uint64_t, ThreadCallTree> &threadTrees() const noexcept { return thread_trees_; }
    const ModuleTable &modules() const noexcept { return modules_; }
    const std::map<std::int32_t, WindowTickStats> &windowTicks() const noexcept { return window_ticks_; }

    std::uint64_t sampleCount() const noexcept { return sample_count_.load(std::memory_order_relaxed); }
    std::uint64_t sampledBytes() const noexcept { return sampled_bytes_.load(std::memory_order_relaxed); }
    std::uint64_t droppedSamples() const noexcept { return dropped_samples_.load(std::memory_order_relaxed); }
    std::uint64_t droppedProfileSamples() const noexcept
    {
        return dropped_profile_samples_.load(std::memory_order_relaxed);
    }
    std::uint64_t pendingSampleDrops() const noexcept { return pending_drops_.load(std::memory_order_relaxed); }
    std::uint64_t pendingCapacityDrops() const noexcept
    {
        return pending_capacity_drops_.load(std::memory_order_relaxed);
    }
    std::uint64_t pendingStaleDrops() const noexcept { return pending_stale_drops_.load(std::memory_order_relaxed); }
    std::uint64_t pendingFinalDrops() const noexcept { return pending_final_drops_.load(std::memory_order_relaxed); }
    static constexpr std::size_t pendingSampleCapacity() noexcept { return kPendingSampleCapacity; }
    std::uint64_t moduleOverflowFrames() const noexcept
    {
        return module_overflow_frames_.load(std::memory_order_relaxed);
    }
    std::uint64_t historySamplesPruned() const noexcept
    {
        return history_samples_pruned_.load(std::memory_order_relaxed);
    }
    std::uint64_t historyBytesPruned() const noexcept { return history_bytes_pruned_.load(std::memory_order_relaxed); }
    std::size_t retainedHistoryWindows() const noexcept { return window_sample_counts_.size(); }
    bool historyTruncated() const noexcept { return historySamplesPruned() != 0; }
    bool profileStorageExhausted() const noexcept { return profile_storage_exhausted_.load(std::memory_order_relaxed); }
    std::size_t profileNodesRemaining() const noexcept { return profile_nodes_remaining_; }
    std::size_t profileTimeEntriesRemaining() const noexcept { return profile_time_entries_remaining_; }
    std::uint64_t threadNameFailures() const noexcept { return thread_filter_.nameFailures(); }
    std::uint64_t threadIdentityCacheDrops() const noexcept { return thread_filter_.cacheDrops(); }
    bool dataIncomplete() const noexcept
    {
        return droppedSamples() != 0 || pendingSampleDrops() != 0 || profileStorageExhausted();
    }

private:
    struct WindowSampleStats {
        std::uint64_t samples = 0;
        std::uint64_t bytes = 0;
    };

    bool admitToTrees(const Sample &sample, CallTree &tree, std::map<std::uint64_t, ThreadCallTree> &thread_trees,
                      std::size_t &remaining_nodes, std::size_t &remaining_time_entries, std::uint64_t *sample_count,
                      std::uint64_t *sampled_bytes) const;
    void recordDrop(std::atomic<std::uint64_t> &counter) noexcept;
    void flushPending(std::uint64_t tick_id, bool keep);
    void pruneHistory(std::int32_t current_window, bool force);
    void pruneTickHistory(std::int32_t current_window);
    void journalThread(const Sample &sample);
    void journalSentinelModule();

    AllocationSamplerConfig config_{};
    AllocationThreadFilter thread_filter_{kNamedThreadRootCapacity, kPendingSampleCapacity};
    ModuleTable modules_{kModuleCapacity};
    CallTree tree_;
    std::map<std::uint64_t, ThreadCallTree> thread_trees_;
    std::map<std::int32_t, WindowTickStats> window_ticks_;
    std::unordered_map<std::uint64_t, std::vector<Sample>> pending_buckets_;
    std::vector<std::uint8_t> tick_decisions_;
    std::map<std::int32_t, WindowSampleStats> window_sample_counts_;
    std::array<bool, kThreadRootCapacity> journaled_thread_roots_{};
    bool journaled_module_sentinel_ = false;
    std::size_t pending_samples_ = 0;
    std::size_t profile_nodes_remaining_ = kProfileNodeCapacity;
    std::size_t profile_time_entries_remaining_ = kProfileTimeEntryCapacity;
    std::int32_t session_start_window_ = 0;
    std::int32_t last_history_window_ = 0;
    std::int32_t last_tick_window_ = 0;
    RecoverySink *recovery_sink_ = nullptr;
    std::atomic<std::uint64_t> sample_count_{0};
    std::atomic<std::uint64_t> sampled_bytes_{0};
    std::atomic<std::uint64_t> dropped_samples_{0};
    std::atomic<std::uint64_t> dropped_profile_samples_{0};
    std::atomic<std::uint64_t> pending_drops_{0};
    std::atomic<std::uint64_t> pending_capacity_drops_{0};
    std::atomic<std::uint64_t> pending_stale_drops_{0};
    std::atomic<std::uint64_t> pending_final_drops_{0};
    std::atomic<std::uint64_t> module_overflow_frames_{0};
    std::atomic<std::uint64_t> history_samples_pruned_{0};
    std::atomic<std::uint64_t> history_bytes_pruned_{0};
    std::atomic<bool> profile_storage_exhausted_{false};
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ALLOCATION_PROFILE_AGGREGATION_H
