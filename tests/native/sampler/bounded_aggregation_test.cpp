#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "native/sampler/call_tree.h"
#include "native/sampler/sampler.h"
#include "native/sampler/types.h"

namespace spark {

struct SamplerTestAccess {
    static bool accept(Sampler &sampler, const Sample &sample) { return sampler.acceptSample(sample); }

    static void reset(Sampler &sampler) { sampler.resetSession(); }

    static void setRemainingStorage(Sampler &sampler, std::size_t nodes, std::size_t time_entries)
    {
        sampler.profile_nodes_remaining_ = nodes;
        sampler.profile_time_entries_remaining_ = time_entries;
    }
};

}  // namespace spark

namespace {

spark::FrameKey frame(std::uint64_t rva)
{
    return {.module = 1, .rva = rva, .raw_address = rva};
}

bool transactionalNodeAndTimeBudget()
{
    spark::CallTree tree;
    const std::vector<spark::FrameKey> frames = {frame(1), frame(2)};
    std::size_t nodes = 1;
    std::size_t times = 2;
    if (tree.logBounded(frames, 0, 7, nodes, times) || !tree.root().times.empty() || !tree.root().children.empty() ||
        nodes != 1 || times != 2) {
        return false;
    }

    nodes = 1;
    if (tree.logBounded(frames, 0, 7, nodes) || !tree.root().times.empty() || !tree.root().children.empty() ||
        nodes != 1) {
        return false;
    }

    nodes = 2;
    times = 3;
    if (!tree.logBounded(frames, 0, 7, nodes, times) || tree.root().times.at(0) != 7 || nodes != 0 || times != 0) {
        return false;
    }

    const spark::CallTree::StorageUsage usage = tree.storageUsage();
    return usage.child_nodes == 2 && usage.time_entries == 3;
}

bool pruningReclaimsExactStorage()
{
    spark::CallTree tree;
    const std::vector<spark::FrameKey> frames = {frame(3)};
    tree.log(frames, 0, 1);
    tree.log(frames, 1, 1);
    if (tree.storageUsage().child_nodes != 1 || tree.storageUsage().time_entries != 4) {
        return false;
    }
    const spark::CallTree::StorageUsage first = tree.pruneBeforeWithUsage(1);
    if (first.child_nodes != 0 || first.time_entries != 2 || tree.storageUsage().time_entries != 2) {
        return false;
    }
    const spark::CallTree::StorageUsage second = tree.pruneBeforeWithUsage(2);
    return second.child_nodes == 1 && second.time_entries == 2 && tree.storageUsage().child_nodes == 0 &&
           tree.storageUsage().time_entries == 0;
}

bool boundedModulesAndSamplerConstants()
{
    spark::ModuleTable modules(3);
    const auto first = modules.intern("first");
    const auto second = modules.intern("second");
    const auto overflow = modules.intern("overflow");
    return first == 1 && second == 2 && overflow == 0 && modules.size() == 3 &&
           spark::Sampler::moduleCapacity() == 512 && spark::Sampler::threadRootCapacity() == 257 &&
           spark::Sampler::pendingSampleCapacity() == 32768 && spark::Sampler::profileNodeCapacity() == 131072 &&
           spark::Sampler::profileTimeEntryCapacity() == 2 * 1024 * 1024;
}

bool combinedTreeBudgetIsTransactional()
{
    spark::Sampler sampler;
    spark::SamplerTestAccess::reset(sampler);
    spark::SamplerTestAccess::setRemainingStorage(sampler, 1, 4);
    spark::Sample sample{.frames = {frame(1)}, .thread_id = 1, .thread_name = "thread", .window = 1};

    return !spark::SamplerTestAccess::accept(sampler, sample) && sampler.tree().root().times.empty() &&
           sampler.threadTrees().empty() && sampler.sampleCount() == 0 && sampler.droppedProfileSamples() == 1 &&
           sampler.droppedSamples() == 1 && sampler.profileStorageExhausted() && sampler.dataIncomplete();
}

bool excessThreadsUseOverflowRoot()
{
    spark::Sampler sampler;
    spark::SamplerTestAccess::reset(sampler);
    for (std::uint64_t thread_id = 1; thread_id <= 257; ++thread_id) {
        spark::Sample sample{
            .frames = {frame(thread_id)}, .thread_id = thread_id, .thread_name = "thread", .window = 1};
        if (!spark::SamplerTestAccess::accept(sampler, sample)) {
            return false;
        }
    }

    const auto overflow = sampler.threadTrees().find(0);
    return sampler.threadTrees().size() == spark::Sampler::threadRootCapacity() &&
           overflow != sampler.threadTrees().end() && overflow->second.thread_name == "<other threads>" &&
           overflow->second.tree.sampleCount() == 1 && sampler.overflowThreadSamples() == 1 &&
           sampler.droppedSamples() == 0 && !sampler.dataIncomplete();
}

}  // namespace

int main()
{
    if (!transactionalNodeAndTimeBudget() || !pruningReclaimsExactStorage() || !boundedModulesAndSamplerConstants() ||
        !combinedTreeBudgetIsTransactional() || !excessThreadsUseOverflowRoot()) {
        std::fprintf(stderr, "bounded aggregation test failed\n");
        return 1;
    }
    return 0;
}
