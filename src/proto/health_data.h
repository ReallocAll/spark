#ifndef ENDSTONE_SPARK_HEALTH_DATA_H
#define ENDSTONE_SPARK_HEALTH_DATA_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/stats/statistics_service.h"
#include "core/stats/system_stats.h"
#include "core/ws/ws_proto.h"

namespace spark {

// Data needed to build a spark `HealthData` protobuf message.
struct HealthData {
    std::string creator_name = "Console";
    bool creator_is_player = false;
    std::string creator_unique_id;
    std::string endstone_version;
    std::string minecraft_version;
    PlatformStats platform_stats;
    WorldInfo world;
    SystemStats system_stats;
    StatisticsSnapshot statistics;
    MetricsSnapshot metrics;
    std::int64_t generated_time_ms = 0;
    std::vector<PluginInfo> plugins;
    std::map<std::string, std::string> server_configurations;
    std::map<std::string, std::string> extra_platform_metadata;
    std::map<std::int32_t, WindowStats> window_stats;
    std::optional<SocketChannelInfo> channel_info;
};

// Serialize a spark `HealthData` protobuf message (uncompressed bytes).
std::string buildHealthData(const HealthData &data);

}  // namespace spark

#endif  // ENDSTONE_SPARK_HEALTH_DATA_H
