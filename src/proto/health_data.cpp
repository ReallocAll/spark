#include "proto/health_data.h"

#include "proto/proto_writer.h"
#include "proto/statistics_proto.h"
#include "spark_constants.h"

namespace spark {

std::string buildHealthData(const HealthData &data)
{
    std::string out;
    ProtoWriter writer(out);

    // HealthMetadata (1)
    {
        std::string metadata;
        ProtoWriter metadata_writer(metadata);

        // creator (1): CommandSenderMetadata
        {
            std::string creator;
            ProtoWriter creator_writer(creator);
            creator_writer.varint(1, data.creator_is_player ? 1 : 0);
            creator_writer.string(2, data.creator_name);
            metadata_writer.message(1, creator);
        }

        // platform_metadata (2): PlatformMetadata
        {
            std::string platform_metadata;
            ProtoWriter platform_writer(platform_metadata);
            platform_writer.varint(1, 0);  // type = SERVER
            platform_writer.string(2, "Endstone");
            platform_writer.string(3, data.endstone_version);
            if (!data.minecraft_version.empty()) {
                platform_writer.string(4, data.minecraft_version);
            }
            platform_writer.int32(7, kSparkFormatVersion);
            platform_writer.string(8, "Endstone");
            metadata_writer.message(2, platform_metadata);
        }

        if (data.platform_stats.present) {
            metadata_writer.message(3, proto_detail::buildPlatformStatistics(data.platform_stats, data.statistics));
        }
        if (data.system_stats.present) {
            metadata_writer.message(4, proto_detail::buildSystemStatistics(data.system_stats, data.statistics));
        }

        // generated_time (5)
        metadata_writer.int64(5, data.generated_time_ms);

        // server_configurations (6): map<string, string>
        for (const auto &[key, value] : data.server_configurations) {
            std::string entry;
            ProtoWriter entry_writer(entry);
            entry_writer.string(1, key);
            entry_writer.string(2, value);
            metadata_writer.message(6, entry);
        }

        // sources (7): map<string, PluginOrModMetadata>
        for (const PluginInfo &plugin : data.plugins) {
            std::string plugin_metadata;
            ProtoWriter plugin_writer(plugin_metadata);
            plugin_writer.string(1, plugin.name);
            if (!plugin.version.empty()) {
                plugin_writer.string(2, plugin.version);
            }
            if (!plugin.author.empty()) {
                plugin_writer.string(3, plugin.author);
            }
            if (!plugin.description.empty()) {
                plugin_writer.string(4, plugin.description);
            }
            std::string entry;
            ProtoWriter entry_writer(entry);
            entry_writer.string(1, plugin.name);
            entry_writer.message(2, plugin_metadata);
            metadata_writer.message(7, entry);
        }

        // extra_platform_metadata (8): map<string, string>
        for (const auto &[key, value] : data.extra_platform_metadata) {
            std::string entry;
            ProtoWriter entry_writer(entry);
            entry_writer.string(1, key);
            entry_writer.string(2, value);
            metadata_writer.message(8, entry);
        }

        writer.message(1, metadata);
    }

    // time_window_statistics (2): map<int32, WindowStatistics>
    for (const auto &[window, stats] : data.window_stats) {
        std::string entry;
        ProtoWriter entry_writer(entry);
        entry_writer.int32(1, window);
        entry_writer.message(2, proto_detail::buildWindowStatistics(stats));
        writer.message(2, entry);
    }

    return out;
}

}  // namespace spark
