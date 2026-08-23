#include <cstdio>
#include <string>

#include "proto/health_data.h"
#include "proto_test_utils.h"

namespace {

bool check(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "health data: %s\n", message);
    }
    return value;
}

}  // namespace

int main()
{
    spark::HealthData data;
    data.creator_name = "Console";
    data.endstone_version = "0.1.0";
    data.minecraft_version = "1.21";
    data.generated_time_ms = 1234;
    data.server_configurations["server.properties"] = R"({"max-players":"20"})";
    data.extra_platform_metadata["sample-count"] = "10";
    data.plugins.push_back({.name = "Example", .version = "1.0"});
    data.platform_stats.present = true;
    data.platform_stats.uptime_ms = 100;

    spark::WindowStats window;
    window.ticks_present = true;
    window.ticks = 12;
    data.window_stats[0] = window;

    const std::string bytes = spark::buildHealthData(data);
    if (!check(spark::proto_test::findMessage(
                   bytes, 1,
                   [&](spark::ProtoReader metadata_reader) {
                       return spark::proto_test::hasVarint(metadata_reader, 5, 1234) &&
                              spark::proto_test::findMessage(
                                  metadata_reader, 6,
                                  [](spark::ProtoReader entry_reader) {
                                      return spark::proto_test::hasString(entry_reader, 1, "server.properties") &&
                                             spark::proto_test::hasString(entry_reader, 2, R"({"max-players":"20"})");
                                  }) &&
                              spark::proto_test::findMessage(metadata_reader, 7, [](spark::ProtoReader entry_reader) {
                                  return spark::proto_test::hasString(entry_reader, 1, "Example");
                              });
                   }),
               "health metadata fields were not encoded") ||
        !check(spark::proto_test::findMessage(
                   bytes, 2,
                   [](spark::ProtoReader entry_reader) {
                       return spark::proto_test::hasVarint(entry_reader, 1, 0) &&
                              spark::proto_test::findMessage(entry_reader, 2, [](spark::ProtoReader window_reader) {
                                  return spark::proto_test::hasVarint(window_reader, 1, 12);
                              });
                   }),
               "health window statistics were not encoded")) {
        return 1;
    }
    return 0;
}
