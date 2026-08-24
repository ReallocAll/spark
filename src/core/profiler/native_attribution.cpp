#include "core/profiler/native_attribution.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>

#include "native/sampler/call_tree.h"
#include "proto/sampler_data.h"

namespace spark {
namespace {

template <std::size_t N>
bool matches(std::string_view method_name, const std::array<std::string_view, N> &known_methods) noexcept
{
    return std::ranges::any_of(
        known_methods, [method_name](const std::string_view known_method) { return method_name == known_method; });
}

constexpr std::array KLinuxMethods{
    std::string_view("spark::AllocationSampler::Impl::hookMalloc(unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookCalloc(unsigned long, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookRealloc(void*, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookFree(void*)"),
    std::string_view("spark::AllocationSampler::Impl::hookReallocArray(void*, unsigned long, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedAlloc(unsigned long, unsigned long)"),
    std::string_view("spark::AllocationSampler::Impl::hookPosixMemalign(void**, unsigned long, unsigned long)"),
};

constexpr std::array KWindowsMethods{
    std::string_view("spark::AllocationSampler::Impl::hookMalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookCalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookRealloc"),
    std::string_view("spark::AllocationSampler::Impl::hookRecalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookFree"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedMalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedRealloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedRecalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedOffsetMalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedOffsetRealloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedOffsetRecalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedFree"),
    std::string_view("spark::AllocationSampler::Impl::hookMallocBase"),
    std::string_view("spark::AllocationSampler::Impl::hookCallocBase"),
    std::string_view("spark::AllocationSampler::Impl::hookReallocBase"),
    std::string_view("spark::AllocationSampler::Impl::hookFreeBase"),
    std::string_view("spark::AllocationSampler::Impl::hookHeapAlloc"),
    std::string_view("spark::AllocationSampler::Impl::hookHeapReAlloc"),
    std::string_view("spark::AllocationSampler::Impl::hookHeapFree"),
};

enum class NodeResult {
    Retained,
    Dropped,
    Malformed,
};

bool addCounts(std::map<std::int32_t, std::uint64_t> &totals, const std::map<std::int32_t, std::uint64_t> &counts)
{
    for (const auto &[window, count] : counts) {
        auto &total = totals[window];
        if (count > std::numeric_limits<std::uint64_t>::max() - total) {
            return false;
        }
        total += count;
    }
    return true;
}

bool isInstrumentation(const FrameKey &key, const ResolvedFrameMap &resolved)
{
    const auto frame = resolved.find(key);
    return frame != resolved.end() && isNativeAllocationInstrumentation(frame->second.method_name);
}

// NOLINTNEXTLINE(misc-no-recursion)
NodeResult copyFilteredNode(const CallTree::Node &source, CallTree::Node &destination, const ResolvedFrameMap &resolved)
{
    if (isInstrumentation(source.key, resolved)) {
        return NodeResult::Dropped;
    }

    destination.key = source.key;
    std::map<std::int32_t, std::uint64_t> original_child_totals;
    std::map<std::int32_t, std::uint64_t> retained_child_totals;
    for (const auto &[key, child] : source.children) {
        if (!addCounts(original_child_totals, child->times)) {
            return NodeResult::Malformed;
        }

        auto filtered_child = std::make_unique<CallTree::Node>();
        const NodeResult result = copyFilteredNode(*child, *filtered_child, resolved);
        if (result == NodeResult::Malformed) {
            return result;
        }
        if (result != NodeResult::Retained) {
            continue;
        }
        if (!addCounts(retained_child_totals, filtered_child->times)) {
            return NodeResult::Malformed;
        }
        destination.children.emplace(key, std::move(filtered_child));
    }

    std::map<std::int32_t, bool> windows;
    for (const auto &[window, count] : source.times) {
        (void)count;
        windows.emplace(window, false);
    }
    for (const auto &[window, count] : original_child_totals) {
        (void)count;
        windows.emplace(window, false);
    }
    for (const auto &[window, count] : retained_child_totals) {
        (void)count;
        windows.emplace(window, false);
    }

    for (const auto &[window, unused] : windows) {
        (void)unused;
        const auto source_count = source.times.find(window);
        const auto original_children = original_child_totals.find(window);
        const auto retained_children = retained_child_totals.find(window);
        const std::uint64_t total = source_count == source.times.end() ? 0 : source_count->second;
        const std::uint64_t child_total =
            original_children == original_child_totals.end() ? 0 : original_children->second;
        const std::uint64_t retained_total =
            retained_children == retained_child_totals.end() ? 0 : retained_children->second;
        if (child_total > total || retained_total > child_total) {
            return NodeResult::Malformed;
        }
        const std::uint64_t self = total - child_total;
        const std::uint64_t filtered_total = self + retained_total;
        if (filtered_total != 0) {
            destination.times.emplace(window, filtered_total);
        }
    }

    return destination.times.empty() && destination.children.empty() ? NodeResult::Dropped : NodeResult::Retained;
}

}  // namespace

bool isNativeAllocationInstrumentation(std::string_view method_name) noexcept
{
    return matches(method_name, KLinuxMethods) || matches(method_name, KWindowsMethods);
}

bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved)
{
    CallTree candidate;
    if (copyFilteredNode(source.root(), candidate.root(), resolved) == NodeResult::Malformed) {
        return false;
    }
    filtered = std::move(candidate);
    return true;
}

void filterExecutionTrees(std::vector<ThreadTreeView> &views, std::vector<std::unique_ptr<CallTree>> &owned_trees,
                          const ResolvedFrameMap &resolved)
{
    for (ThreadTreeView &view : views) {
        if (view.tree == nullptr) {
            continue;
        }
        auto filtered = std::make_unique<CallTree>();
        if (!filterExecutionTree(*filtered, *view.tree, resolved)) {
            mergeCallTree(*filtered, *view.tree);
        }
        view.tree = filtered.get();
        owned_trees.push_back(std::move(filtered));
    }
}

}  // namespace spark
