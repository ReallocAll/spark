#ifndef ENDSTONE_SPARK_SAMPLER_DATA_H
#define ENDSTONE_SPARK_SAMPLER_DATA_H

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/profiler/profile_mode.h"
#include "core/stats/statistics_service.h"
#include "core/stats/system_stats.h"
#include "native/python/python_profile_bridge.h"
#include "native/sampler/call_tree.h"
#include "native/sampler/types.h"
#include "native/symbol/symbolicate.h"
#include "proto/health_data.h"

namespace spark {

struct ThreadTreeView {
    std::string_view name;
    const CallTree *tree = nullptr;
};

// Everything about the run that isn't the call tree itself.
struct ProfileMetadata {
    ProfileMetadata()
    {
        PythonStackProvider *provider = globalPythonStackProvider();
        if (provider == nullptr) {
            return;
        }
        const PythonAttributionExport python = provider->exportState();
        const PythonAttributionDiagnostics &diag = python.diagnostics;
        extra_platform_metadata["Python attribution supported"] = diag.supported ? "true" : "false";
        extra_platform_metadata["Python attribution backend"] = pythonJsonString(diag.backend);
        extra_platform_metadata["Python version"] = pythonJsonString(diag.python_version);
        extra_platform_metadata["Python function attribution enabled"] =
            diag.supported && diag.monitoring_active ? "true" : "false";
        extra_platform_metadata["Python monitoring active"] = diag.monitoring_active ? "true" : "false";
        if (!diag.unavailable_reason.empty()) {
            extra_platform_metadata["Python attribution unavailable reason"] =
                pythonJsonString(diag.unavailable_reason);
        }
        extra_platform_metadata["Python PY_START events"] = std::to_string(diag.py_start);
        extra_platform_metadata["Python PY_RESUME events"] = std::to_string(diag.py_resume);
        extra_platform_metadata["Python PY_THROW events"] = std::to_string(diag.py_throw);
        extra_platform_metadata["Python PY_RETURN events"] = std::to_string(diag.py_return);
        extra_platform_metadata["Python PY_YIELD events"] = std::to_string(diag.py_yield);
        extra_platform_metadata["Python PY_UNWIND events"] = std::to_string(diag.py_unwind);
        extra_platform_metadata["Python registered threads"] = std::to_string(diag.registered_threads);
        extra_platform_metadata["Python shadow max depth"] = std::to_string(diag.max_depth);
        extra_platform_metadata["Python shadow depth capacity"] = std::to_string(PythonStackProvider::kMaxDepth);
        extra_platform_metadata["Python shadow overflows"] = std::to_string(diag.overflows);
        extra_platform_metadata["Python shadow snapshot attempts"] = std::to_string(diag.snapshot_attempts);
        extra_platform_metadata["Python shadow snapshot failures"] = std::to_string(diag.snapshot_failures);
        extra_platform_metadata["Python attributed samples"] = std::to_string(diag.attribution_samples);
        extra_platform_metadata["Python native-only samples"] = std::to_string(diag.native_only_samples);
        extra_platform_metadata["Python native boundary misses"] = std::to_string(diag.boundary_misses);
        extra_platform_metadata["Python thread mismatches"] = std::to_string(diag.thread_mismatches);
        extra_platform_metadata["Python unknown code IDs"] = std::to_string(diag.unknown_code_ids);
        extra_platform_metadata["Python code objects"] = std::to_string(diag.code_objects);
        extra_platform_metadata["Python plugin code objects"] = std::to_string(diag.plugin_code);
        extra_platform_metadata["Python stdlib code objects"] = std::to_string(diag.stdlib_code);
        extra_platform_metadata["Python external code objects"] = std::to_string(diag.external_code);
        extra_platform_metadata["Python Endstone code objects"] = std::to_string(diag.endstone_code);
        extra_platform_metadata["Python unknown code objects"] = std::to_string(diag.unknown_code);
        extra_platform_metadata["Python code cache hits"] = std::to_string(diag.code_cache_hits);
        extra_platform_metadata["Python code cache misses"] = std::to_string(diag.code_cache_misses);
        extra_platform_metadata["Python monitoring callback failures"] =
            std::to_string(diag.monitoring_callbacks_failed);
        extra_platform_metadata["Python frame representation"] =
            pythonJsonString("class=[Python] module; method=qualname; descriptor=filename; line=co_firstlineno");

        for (const PythonCodeMetadata &code : python.codes) {
            python_codes.emplace(code.code_id, code);
            if (code.category != PythonCodeCategory::Plugin || code.plugin_source.empty()) {
                continue;
            }
            const std::string class_name = pythonFrameClassName(code);
            const auto [it, inserted] = class_sources.emplace(class_name, code.plugin_source);
            if (!inserted && it->second != code.plugin_source) {
                class_sources.erase(it);
            }
        }
    }

    std::int64_t start_time_ms = 0;
    std::int64_t end_time_ms = 0;
    std::int32_t interval = 4000;  // execution: microseconds; allocation: bytes
    ProfileMode mode = ProfileMode::Execution;
    std::int32_t number_of_ticks = 0;
    std::string endstone_version;
    std::string minecraft_version;
    std::string engine_version;  // e.g. "endstone-spark 0.1.0"
    std::string comment;
    std::string creator_name = "Console";
    bool creator_is_player = false;
    std::string creator_unique_id;
    std::string thread_name = "Server thread";
    bool all_threads = false;
    bool regex_threads = false;
    std::vector<std::int64_t> thread_ids;
    std::vector<std::string> thread_patterns;
    bool ticked = false;  // --only-ticks-over active
    std::int64_t tick_threshold_us = 0;
    std::int32_t number_of_included_ticks = 0;
    ThreadGrouperMode thread_grouper = ThreadGrouperMode::ByPool;
    PlatformStats platform_stats;
    SystemStats system_stats;
    StatisticsSnapshot statistics;
    MetricsSnapshot metrics;
    std::map<std::int32_t, WindowStats> window_stats;
    std::map<std::string, std::string> extra_platform_metadata;
    std::map<std::string, std::string> server_configurations;
    std::vector<PluginInfo> plugins;
    std::map<std::string, std::string> class_sources;
    std::unordered_map<PythonCodeId, PythonCodeMetadata> python_codes;
    WorldInfo world;
    std::string socket_channel_info_proto;  // field 8: SocketChannelInfo (empty for non-live)
};

// Collect every distinct frame key present in the tree (for batch symbolication).
std::vector<FrameKey> collectFrameKeys(const CallTree &tree);
std::vector<FrameKey> collectFrameKeys(const std::vector<ThreadTreeView> &threads);

// Serialize a spark `SamplerData` protobuf message (uncompressed bytes).
std::string buildSamplerData(const ProfileMetadata &meta, const CallTree &tree,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved);
std::string buildSamplerData(const ProfileMetadata &meta, const std::vector<ThreadTreeView> &threads,
                             const std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> &resolved);

}  // namespace spark

#endif  // ENDSTONE_SPARK_SAMPLER_DATA_H
