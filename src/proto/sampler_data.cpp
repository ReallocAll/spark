#include "proto/sampler_data.h"

#include <algorithm>
#include <set>
#include <unordered_set>

#include "profiling_window.h"
#include "proto/metrics_proto.h"
#include "proto/proto_writer.h"
#include "proto/statistics_proto.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::uint64_t nodeTotal(const CallTree::Node &n)
{
    std::uint64_t total = 0;
    for (const auto &[window, count] : n.times) {
        total += count;
    }
    return total;
}

std::vector<double> alignValues(const std::map<std::int32_t, std::uint64_t> &times,
                                const std::vector<std::int32_t> &windows, const ProfileMetadata &meta)
{
    std::vector<double> out(windows.size(), 0.0);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        auto it = times.find(windows[i]);
        if (it == times.end()) {
            continue;
        }
        if (meta.mode == ProfileMode::Allocation) {
            out[i] = static_cast<double>(it->second);
        }
        else {
            out[i] = static_cast<double>(it->second) / 1000.0;
        }
    }
    return out;
}

std::vector<const CallTree::Node *> sortedChildren(const CallTree::Node &node)
{
    std::vector<const CallTree::Node *> kids;
    kids.reserve(node.children.size());
    for (const auto &[key, child] : node.children) {
        kids.push_back(child.get());
    }
    std::ranges::sort(kids, [](const CallTree::Node *a, const CallTree::Node *b) {
        std::uint64_t ta = nodeTotal(*a);
        std::uint64_t tb = nodeTotal(*b);
        return ta != tb ? ta > tb : a->key.rva < b->key.rva;
    });
    return kids;
}

// Post-order flatten: append children first, then this node; return this node's index.
// NOLINTNEXTLINE(misc-no-recursion)
int emitNode(const CallTree::Node *node, const std::vector<std::int32_t> &windows, const ProfileMetadata &meta,
             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved, std::vector<std::string> &flat)
{
    std::vector<std::int32_t> child_refs;
    for (const CallTree::Node *kid : sortedChildren(*node)) {
        child_refs.push_back(emitNode(kid, windows, meta, resolved, flat));
    }

    std::string bytes;
    ProtoWriter w(bytes);
    auto it = resolved.find(node->key);
    if (it != resolved.end()) {
        w.string(3, it->second.class_name);
        w.string(4, it->second.method_name);
        if (it->second.line >= 0) {
            w.int32(6, it->second.line);
        }
        if (!it->second.method_desc.empty()) {
            w.string(7, it->second.method_desc);
        }
        // Private extension fields: preserve sampled PC and unwind root for offline evaluation.
        w.varint(1001, node->key.rva);
        if (it->second.guessed_function_rva != 0) {
            w.varint(1002, it->second.guessed_function_rva);
        }
    }
    w.packedDouble(8, alignValues(node->times, windows, meta));
    w.packedInt32(9, child_refs);

    flat.push_back(std::move(bytes));
    return static_cast<int>(flat.size()) - 1;
}

std::string buildMetadata(const ProfileMetadata &m)
{
    std::string out;
    ProtoWriter w(out);

    // creator (1): CommandSenderMetadata { type, name }
    {
        std::string c;
        ProtoWriter cw(c);
        cw.varint(1, m.creator_is_player ? 1 : 0);
        cw.string(2, m.creator_name);
        if (m.creator_is_player && !m.creator_unique_id.empty()) {
            cw.string(3, m.creator_unique_id);
        }
        w.message(1, c);
    }
    w.int64(2, m.start_time_ms);
    w.int32(3, m.interval);
    // thread_dumper (4): { type = ALL/SPECIFIC/REGEX, ids, patterns }
    {
        std::string t;
        ProtoWriter tw(t);
        int dumper_type = 1;
        if (m.all_threads) {
            dumper_type = 0;
        }
        else if (m.regex_threads) {
            dumper_type = 2;
        }
        tw.varint(1, dumper_type);
        for (std::int64_t id : m.thread_ids) {
            tw.int64(2, id);
        }
        for (const std::string &pattern : m.thread_patterns) {
            tw.string(3, pattern);
        }
        w.message(4, t);
    }
    // data_aggregator (5): { type, thread_grouper, tick_length_threshold }
    {
        std::string d;
        ProtoWriter dw(d);
        dw.varint(1, m.ticked ? 1 : 0);
        dw.varint(2, static_cast<std::int32_t>(m.thread_grouper));
        if (m.ticked) {
            dw.int64(3, m.tick_threshold_us);
            if (m.number_of_included_ticks > 0) {
                dw.int32(4, m.number_of_included_ticks);
            }
        }
        w.message(5, d);
    }
    if (!m.comment.empty()) {
        w.string(6, m.comment);
    }
    // platform_metadata (7)
    {
        std::string p;
        ProtoWriter pw(p);
        pw.varint(1, 0);  // type = SERVER
        pw.string(2, "Endstone");
        pw.string(3, m.endstone_version);
        if (!m.minecraft_version.empty()) {
            pw.string(4, m.minecraft_version);
        }
        pw.int32(7, kSparkFormatVersion);  // spark_version (gates viewer feature support)
        pw.string(8, "Endstone");          // brand
        w.message(7, p);
    }
    w.int64(11, m.end_time_ms);
    w.int32(12, m.number_of_ticks);
    w.varint(15, m.mode == ProfileMode::Allocation ? 1 : 0);
    w.varint(16, 1);  // sampler_engine = ASYNC
    if (!m.engine_version.empty()) {
        w.string(17, m.engine_version);
    }

    // platform_statistics (8)
    if (m.platform_stats.present) {
        w.message(8, proto_detail::buildPlatformStatistics(m.platform_stats, m.statistics, &m.world));
    }

    // system_statistics (9)
    if (m.system_stats.present) {
        w.message(9, proto_detail::buildSystemStatistics(m.system_stats, m.statistics));
    }

    // sources (13): map<string, PluginOrModMetadata> — the loaded plugins
    for (const PluginInfo &pl : m.plugins) {
        std::string pm;
        ProtoWriter pmw(pm);
        pmw.string(1, pl.name);
        if (!pl.version.empty()) {
            pmw.string(2, pl.version);
        }
        if (!pl.author.empty()) {
            pmw.string(3, pl.author);
        }
        if (!pl.description.empty()) {
            pmw.string(4, pl.description);
        }
        std::string entry;
        ProtoWriter ew(entry);
        ew.string(1, pl.name);
        ew.message(2, pm);
        w.message(13, entry);
    }

    // extra_platform_metadata (14): map<string, string>. This is the only
    // extensible metadata surface in the upstream spark schema, so native-only
    // diagnostics such as captured/dropped allocation samples are placed here.
    for (const auto &[key, value] : m.extra_platform_metadata) {
        std::string entry;
        ProtoWriter ew(entry);
        ew.string(1, key);
        ew.string(2, value);
        w.message(14, entry);
    }
    // server_configurations (10): map<string, string> - allowlisted
    // server.properties entries for performance diagnostics.
    for (const auto &[key, value] : m.server_configurations) {
        std::string entry;
        ProtoWriter ew(entry);
        ew.string(1, key);
        ew.string(2, value);
        w.message(10, entry);
    }
    if (!m.metrics.empty()) {
        w.message(18, proto_detail::buildMetrics(m.metrics));
    }
    return out;
}

}  // namespace

std::vector<FrameKey> collectFrameKeys(const CallTree &tree)
{
    return collectFrameKeys({ThreadTreeView{.name = "", .tree = &tree}});
}

std::vector<FrameKey> collectFrameKeys(const std::vector<ThreadTreeView> &threads)
{
    std::vector<FrameKey> keys;
    std::unordered_set<FrameKey, FrameKeyHash> seen;
    std::vector<const CallTree::Node *> stack;
    for (const ThreadTreeView &thread : threads) {
        if (thread.tree == nullptr) {
            continue;
        }
        for (const auto &[key, child] : thread.tree->root().children) {
            stack.push_back(child.get());
        }
    }
    while (!stack.empty()) {
        const CallTree::Node *node = stack.back();
        stack.pop_back();
        if (seen.insert(node->key).second) {
            keys.push_back(node->key);
        }
        for (const auto &[key, child] : node->children) {
            stack.push_back(child.get());
        }
    }
    return keys;
}

std::string buildSamplerData(const ProfileMetadata &meta, const CallTree &tree,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved)
{
    return buildSamplerData(meta, {ThreadTreeView{.name = meta.thread_name, .tree = &tree}}, resolved);
}

std::string buildSamplerData(const ProfileMetadata &meta, const std::vector<ThreadTreeView> &threads,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved)
{
    // The viewer requires a WindowStatistics entry for every time window, so the window
    // set is the union of windows that have samples and windows that have tick stats.
    std::set<std::int32_t> window_set;
    for (const ThreadTreeView &thread : threads) {
        if (thread.tree == nullptr) {
            continue;
        }
        for (const auto &[window, count] : thread.tree->root().times) {
            window_set.insert(window);
        }
    }
    for (const auto &[window, ws] : meta.window_stats) {
        window_set.insert(window);
    }
    if (window_set.empty()) {
        window_set.insert(0);
    }
    std::vector<std::int32_t> windows(window_set.begin(), window_set.end());

    std::string out;
    ProtoWriter w(out);
    w.message(1, buildMetadata(meta));
    for (const ThreadTreeView &thread_view : threads) {
        if (thread_view.tree == nullptr) {
            continue;
        }
        std::vector<std::string> flat;
        std::vector<std::int32_t> top_refs;
        for (const CallTree::Node *kid : sortedChildren(thread_view.tree->root())) {
            top_refs.push_back(emitNode(kid, windows, meta, resolved, flat));
        }

        std::string thread;
        ProtoWriter tw(thread);
        tw.string(1, thread_view.name);
        for (const std::string &node_bytes : flat) {
            tw.message(3, node_bytes);
        }
        tw.packedDouble(4, alignValues(thread_view.tree->root().times, windows, meta));
        tw.packedInt32(5, top_refs);
        w.message(2, thread);
    }
    for (const auto &[class_name, source_id] : meta.class_sources) {
        std::string entry;
        ProtoWriter ew(entry);
        ew.string(1, class_name);
        ew.string(2, source_id);
        w.message(3, entry);
    }
    w.packedInt32(6, windows);

    // time_window_statistics (7): map<int32, WindowStatistics> — one per time window.
    for (std::int32_t window : windows) {
        auto it = meta.window_stats.find(window);
        WindowStats ws = it != meta.window_stats.end() ? it->second : WindowStats{};
        if (it == meta.window_stats.end()) {
            // Upstream spark emits a zeroed WindowStatistics entry with the
            // profiling window duration when measured statistics are missing.
            ws.duration_ms = static_cast<int>(profiling_window::kSizeMs);
        }
        std::string entry;
        ProtoWriter ew(entry);
        ew.int32(1, window);
        ew.message(2, proto_detail::buildWindowStatistics(ws));
        w.message(7, entry);
    }

    // socket_channel_info (8): SocketChannelInfo for live viewer.
    if (!meta.socket_channel_info_proto.empty()) {
        w.message(8, meta.socket_channel_info_proto);
    }

    return out;
}

}  // namespace spark
