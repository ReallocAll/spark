#include "core/profiler/native_attribution.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>

#include "native/sampler/call_tree.h"
#include "proto/sampler_data.h"

namespace spark {
namespace {

constexpr std::array KInstrumentationMethods{
    std::string_view("spark::AllocationSampler::Impl::hookMalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookCalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookRealloc"),
    std::string_view("spark::AllocationSampler::Impl::hookRecalloc"),
    std::string_view("spark::AllocationSampler::Impl::hookFree"),
    std::string_view("spark::AllocationSampler::Impl::hookReallocArray"),
    std::string_view("spark::AllocationSampler::Impl::hookAlignedAlloc"),
    std::string_view("spark::AllocationSampler::Impl::hookPosixMemalign"),
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

bool matchesInstrumentationMethod(std::string_view method_name, std::string_view known_method) noexcept
{
    if (method_name == known_method) {
        return true;
    }
    return method_name.size() > known_method.size() + 1 && method_name.starts_with(known_method) &&
           method_name[known_method.size()] == '(' && method_name.back() == ')';
}

bool unresolvedMethod(std::string_view method_name) noexcept
{
    return method_name.empty() || method_name.starts_with("0x");
}

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

bool isInstrumentation(const FrameKey &key, const ResolvedFrameMap &resolved,
                       const std::vector<AllocationInstrumentationRange> &ranges)
{
    if (!isNativeAllocationInstrumentationAddress(key.raw_address, ranges)) {
        return false;
    }
    const auto frame = resolved.find(key);
    if (frame == resolved.end() || unresolvedMethod(frame->second.method_name)) {
        return true;
    }
    return isNativeAllocationInstrumentation(frame->second.method_name);
}

// NOLINTNEXTLINE(misc-no-recursion)
NodeResult copyFilteredNode(const CallTree::Node &source, CallTree::Node &destination, const ResolvedFrameMap &resolved,
                            const std::vector<AllocationInstrumentationRange> &ranges)
{
    if (isInstrumentation(source.key, resolved, ranges)) {
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
        const NodeResult result = copyFilteredNode(*child, *filtered_child, resolved, ranges);
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
    return std::ranges::any_of(KInstrumentationMethods, [method_name](std::string_view known_method) {
        return matchesInstrumentationMethod(method_name, known_method);
    });
}

bool isNativeAllocationInstrumentationAddress(
    std::uint64_t raw_address, const std::vector<AllocationInstrumentationRange> &ranges) noexcept
{
    if (raw_address == 0) {
        return false;
    }
    return std::ranges::any_of(ranges, [raw_address](const AllocationInstrumentationRange &range) {
        return range.begin < range.end && raw_address >= range.begin && raw_address < range.end;
    });
}

bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved,
                         const std::vector<AllocationInstrumentationRange> &ranges)
{
    CallTree candidate;
    if (copyFilteredNode(source.root(), candidate.root(), resolved, ranges) == NodeResult::Malformed) {
        return false;
    }
    filtered = std::move(candidate);
    return true;
}

bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved)
{
    return filterExecutionTree(filtered, source, resolved, allocationInstrumentationRanges());
}

void filterExecutionTrees(std::vector<ThreadTreeView> &views, std::vector<std::unique_ptr<CallTree>> &owned_trees,
                          const ResolvedFrameMap &resolved)
{
    const auto ranges = allocationInstrumentationRanges();
    for (ThreadTreeView &view : views) {
        if (view.tree == nullptr) {
            continue;
        }
        auto filtered = std::make_unique<CallTree>();
        if (!filterExecutionTree(*filtered, *view.tree, resolved, ranges)) {
            mergeCallTree(*filtered, *view.tree);
        }
        view.tree = filtered.get();
        owned_trees.push_back(std::move(filtered));
    }
}

}  // namespace spark
