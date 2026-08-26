#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/profiler/native_attribution.h"
#include "core/profiler/profiler.h"

namespace spark {

struct ProfilerTestAccess {
    static void addNativePluginSources(ProfileMetadata &meta, const ExportContext &ctx,
                                       const std::vector<FrameKey> &keys,
                                       const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved)
    {
        Profiler::addNativePluginSources(meta, ctx, keys, resolved);
    }
};

}  // namespace spark

namespace {

std::uint64_t readVarint(const std::string &data, std::size_t &offset)
{
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        assert(offset < data.size());
        const auto byte = static_cast<std::uint8_t>(data[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    assert(false);
    return 0;
}

std::string readBytes(const std::string &data, std::size_t &offset)
{
    const auto size = static_cast<std::size_t>(readVarint(data, offset));
    assert(size <= data.size() - offset);
    std::string value = data.substr(offset, size);
    offset += size;
    return value;
}

void skipField(const std::string &data, std::size_t &offset, std::uint64_t wire_type)
{
    if (wire_type == 0) {
        static_cast<void>(readVarint(data, offset));
    }
    else if (wire_type == 1) {
        assert(8 <= data.size() - offset);
        offset += 8;
    }
    else if (wire_type == 2) {
        static_cast<void>(readBytes(data, offset));
    }
    else if (wire_type == 5) {
        assert(4 <= data.size() - offset);
        offset += 4;
    }
    else {
        assert(false);
    }
}

std::map<std::string, std::string> classSources(const std::string &payload)
{
    std::map<std::string, std::string> result;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const std::uint64_t tag = readVarint(payload, offset);
        if ((tag >> 3) != 3) {
            skipField(payload, offset, tag & 7);
            continue;
        }

        const std::string entry = readBytes(payload, offset);
        std::size_t entry_offset = 0;
        std::string class_name;
        std::string source_id;
        while (entry_offset < entry.size()) {
            const std::uint64_t entry_tag = readVarint(entry, entry_offset);
            if ((entry_tag >> 3) == 1) {
                class_name = readBytes(entry, entry_offset);
            }
            else if ((entry_tag >> 3) == 2) {
                source_id = readBytes(entry, entry_offset);
            }
            else {
                skipField(entry, entry_offset, entry_tag & 7);
            }
        }
        result.emplace(std::move(class_name), std::move(source_id));
    }
    return result;
}

struct NodeFields {
    std::string class_name;
    std::string method_name;
    std::uint64_t rva = 0;
};

NodeFields nodeFields(const std::string &node)
{
    NodeFields result;
    std::size_t offset = 0;
    while (offset < node.size()) {
        const std::uint64_t tag = readVarint(node, offset);
        if ((tag >> 3) == 3) {
            result.class_name = readBytes(node, offset);
        }
        else if ((tag >> 3) == 4) {
            result.method_name = readBytes(node, offset);
        }
        else if ((tag >> 3) == 1001) {
            result.rva = readVarint(node, offset);
        }
        else {
            skipField(node, offset, tag & 7);
        }
    }
    return result;
}

std::vector<NodeFields> serializedNodes(const std::string &payload)
{
    std::vector<NodeFields> result;
    std::size_t payload_offset = 0;
    while (payload_offset < payload.size()) {
        const std::uint64_t payload_tag = readVarint(payload, payload_offset);
        if ((payload_tag >> 3) != 2) {
            skipField(payload, payload_offset, payload_tag & 7);
            continue;
        }

        const std::string thread = readBytes(payload, payload_offset);
        std::size_t thread_offset = 0;
        while (thread_offset < thread.size()) {
            const std::uint64_t thread_tag = readVarint(thread, thread_offset);
            if ((thread_tag >> 3) == 3) {
                result.push_back(nodeFields(readBytes(thread, thread_offset)));
            }
            else {
                skipField(thread, thread_offset, thread_tag & 7);
            }
        }
    }
    return result;
}

spark::FrameKey frame(std::uint32_t module, std::uintptr_t base, std::uint64_t rva)
{
    return {.module = module, .rva = rva, .raw_address = base + rva};
}

}  // namespace

int main()
{
    assert(spark::isNativeAllocationInstrumentation("spark::AllocationSampler::Impl::hookMalloc(unsigned long)"));
    assert(spark::isNativeAllocationInstrumentation("spark::AllocationSampler::Impl::hookHeapAlloc"));
    assert(spark::isNativeAllocationInstrumentation("spark::AllocationSampler::Impl::hookMalloc"));
    assert(spark::isNativeAllocationInstrumentation("spark::AllocationSampler::Impl::hookMallocBase"));
    assert(spark::isNativeAllocationInstrumentation("_ZN5spark17AllocationSampler4Impl10hookMallocEm"));
    assert(spark::isNativeAllocationInstrumentation("_ZN5spark17AllocationSampler4Impl17hookPosixMemalignEPPvmm"));
    assert(!spark::isNativeAllocationInstrumentation("spark::AllocationSampler::Impl::hookMallocExtra(unsigned long)"));
    assert(!spark::isNativeAllocationInstrumentation("plugin::AllocationSampler::Impl::hookMalloc(unsigned long)"));

    spark::ExportContext context;
    context.native_plugin_sources = {
        {.module_base = 0x100000, .module_path = "plugin-a", .source_id = "plugin_a"},
        {.module_base = 0x200000, .module_path = "plugin-b", .source_id = "plugin_b"},
    };

    const spark::FrameKey root_frame = frame(1, 0x100000, 0x100);
    const spark::FrameKey parent_frame = frame(1, 0x100000, 0x200);
    const spark::FrameKey instrumentation = frame(1, 0x100000, 0x300);
    const spark::FrameKey unresolved_instrumentation = frame(1, 0x100000, 0x308);
    const spark::FrameKey instrumentation_leaf = frame(1, 0x100000, 0x310);
    const spark::FrameKey ordinary_inside_range = frame(1, 0x100000, 0x318);
    const spark::FrameKey ordinary_leaf = frame(1, 0x100000, 0x400);
    const spark::FrameKey sibling = frame(1, 0x100000, 0x500);
    const spark::FrameKey sibling_leaf = frame(1, 0x100000, 0x510);
    const spark::FrameKey exact_name_outside_range = frame(1, 0x100000, 0x600);
    const std::vector<spark::NativeInstrumentationRange> instrumentation_ranges{
        {.begin = 0x1002f0, .end = 0x100320},
    };
    assert(spark::isNativeAllocationInstrumentationAddress(0x1002f0, instrumentation_ranges));
    assert(spark::isNativeAllocationInstrumentationAddress(0x10031f, instrumentation_ranges));
    assert(!spark::isNativeAllocationInstrumentationAddress(0x100320, instrumentation_ranges));
    assert(!spark::isNativeAllocationInstrumentationAddress(ordinary_leaf.raw_address, instrumentation_ranges));

    spark::ResolvedFrameMap resolved = {
        {root_frame, {.class_name = "plugin-a.dll", .method_name = "root"}},
        {parent_frame, {.class_name = "plugin-a.dll", .method_name = "ordinaryParent"}},
        {instrumentation, {.class_name = "plugin-a.dll", .method_name = "spark::AllocationSampler::Impl::hookMalloc"}},
        {unresolved_instrumentation, {.class_name = "plugin-a.dll", .method_name = "0x308"}},
        {instrumentation_leaf, {.class_name = "plugin-a.dll", .method_name = "hookDescendant"}},
        {ordinary_inside_range, {.class_name = "plugin-a.dll", .method_name = "ordinaryInsideRange"}},
        {ordinary_leaf, {.class_name = "plugin-a.dll", .method_name = "ordinaryLeaf"}},
        {sibling, {.class_name = "plugin-a.dll", .method_name = "ordinarySibling"}},
        {sibling_leaf, {.class_name = "plugin-a.dll", .method_name = "siblingLeaf"}},
        {exact_name_outside_range,
         {.class_name = "plugin-a.dll", .method_name = "spark::AllocationSampler::Impl::hookMalloc"}},
    };

    spark::CallTree tree;
    tree.log({parent_frame, root_frame}, 1, 2);
    tree.log({ordinary_leaf, parent_frame, root_frame}, 1, 3);
    tree.log({instrumentation_leaf, instrumentation, parent_frame, root_frame}, 1, 5);
    tree.log({sibling_leaf, sibling, root_frame}, 1, 11);
    tree.log({parent_frame, root_frame}, 2, 4);
    tree.log({ordinary_leaf, parent_frame, root_frame}, 2, 6);
    tree.log({instrumentation_leaf, instrumentation, parent_frame, root_frame}, 2, 7);
    tree.log({sibling_leaf, sibling, root_frame}, 2, 5);

    spark::CallTree filtered;
    assert(spark::filterExecutionTree(filtered, tree, resolved, instrumentation_ranges));
    assert(filtered.root().times.at(1) == 16);
    assert(filtered.root().times.at(2) == 15);
    const auto &filtered_root = *filtered.root().children.at(root_frame);
    assert(filtered_root.times.at(1) == 16);
    assert(filtered_root.times.at(2) == 15);
    const auto &filtered_parent = *filtered_root.children.at(parent_frame);
    assert(filtered_parent.times.at(1) == 5);
    assert(filtered_parent.times.at(2) == 10);
    assert(!filtered_parent.children.contains(instrumentation));
    assert(filtered_parent.children.at(ordinary_leaf)->times.at(1) == 3);
    assert(filtered_parent.children.at(ordinary_leaf)->times.at(2) == 6);
    assert(filtered_root.children.at(sibling)->times.at(1) == 11);
    assert(filtered_root.children.at(sibling)->times.at(2) == 5);

    const auto filtered_keys = spark::collectFrameKeys(filtered);
    assert(std::ranges::find(filtered_keys, instrumentation) == filtered_keys.end());
    assert(std::ranges::find(filtered_keys, instrumentation_leaf) == filtered_keys.end());
    assert(std::ranges::find(filtered_keys, sibling) != filtered_keys.end());

    spark::CallTree unresolved_tree;
    unresolved_tree.log({unresolved_instrumentation, root_frame}, 1, 4);
    spark::CallTree unresolved_filtered;
    assert(spark::filterExecutionTree(unresolved_filtered, unresolved_tree, resolved, instrumentation_ranges));
    assert(spark::collectFrameKeys(unresolved_filtered).empty());

    spark::ResolvedFrameMap missing_unresolved = resolved;
    missing_unresolved.erase(unresolved_instrumentation);
    spark::CallTree missing_unresolved_filtered;
    assert(spark::filterExecutionTree(missing_unresolved_filtered, unresolved_tree, missing_unresolved,
                                      instrumentation_ranges));
    assert(spark::collectFrameKeys(missing_unresolved_filtered).empty());

    spark::CallTree guarded_tree;
    guarded_tree.log({ordinary_inside_range, root_frame}, 1, 3);
    guarded_tree.log({exact_name_outside_range, root_frame}, 1, 5);
    spark::CallTree guarded_filtered;
    assert(spark::filterExecutionTree(guarded_filtered, guarded_tree, resolved, instrumentation_ranges));
    const auto guarded_keys = spark::collectFrameKeys(guarded_filtered);
    assert(std::ranges::find(guarded_keys, ordinary_inside_range) != guarded_keys.end());
    assert(std::ranges::find(guarded_keys, exact_name_outside_range) != guarded_keys.end());

    spark::ProfileMetadata metadata;
    spark::ProfilerTestAccess::addNativePluginSources(metadata, context, filtered_keys, resolved);
    const std::map<std::string, std::string> expected_sources{{"plugin-a.dll", "plugin_a"}};
    assert(metadata.class_sources == expected_sources);

    spark::ProfileMetadata allocation_metadata;
    spark::ProfilerTestAccess::addNativePluginSources(allocation_metadata, context, spark::collectFrameKeys(tree),
                                                      resolved);
    assert(allocation_metadata.class_sources == expected_sources);

    const std::string payload = spark::buildSamplerData(metadata, filtered, resolved);
    assert(classSources(payload) == metadata.class_sources);
    const auto nodes = serializedNodes(payload);
    assert(nodes.size() == 5);
    for (const NodeFields &node : nodes) {
        assert(node.method_name != resolved.at(instrumentation).method_name);
        assert(node.rva != instrumentation.rva);
    }

    spark::CallTree malformed;
    malformed.root().times.emplace(1, 1);
    auto malformed_child = std::make_unique<spark::CallTree::Node>();
    malformed_child->key = root_frame;
    malformed_child->times.emplace(1, 2);
    malformed.root().children.emplace(root_frame, std::move(malformed_child));
    spark::CallTree malformed_filtered;
    assert(!spark::filterExecutionTree(malformed_filtered, malformed, resolved, instrumentation_ranges));
    assert(malformed_filtered.root().times.empty());

    const spark::FrameKey conflicting = frame(3, 0x200000, 0x320);
    resolved.emplace(conflicting, spark::ResolvedFrame{.class_name = "plugin-a.dll", .method_name = "conflict"});
    spark::ProfilerTestAccess::addNativePluginSources(metadata, context, {conflicting}, resolved);
    assert(metadata.class_sources.empty());

    const spark::FrameKey underflow{.module = 4, .rva = 0x200, .raw_address = 0x100};
    resolved.emplace(underflow, spark::ResolvedFrame{.class_name = "invalid", .method_name = "invalid"});
    spark::ProfilerTestAccess::addNativePluginSources(metadata, context, {underflow}, resolved);
    assert(metadata.class_sources.empty());
}
