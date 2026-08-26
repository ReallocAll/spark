#include <cstdio>
#include <string>

#include "core/stats/statistics_service.h"
#include "proto/statistics_proto.h"
#include "proto_test_utils.h"

namespace {

bool check(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "statistics proto: %s\n", message);
    }
    return value;
}

}  // namespace

int main()
{
    spark::PlatformStats platform;
    platform.process_mem_present = true;
    platform.process_mem_bytes = 1024;
    platform.uptime_ms = 2000;
    platform.player_count = 4;
    platform.online_mode = 2;

    spark::StatisticsSnapshot statistics;
    statistics.tps.last_1m = {.present = true, .value = 19.0};
    statistics.cpu.process_last_1m = {.present = true, .value = 0.25};

    spark::WorldInfo world;
    world.present = true;
    world.total_entities = 8;
    world.game_rules.push_back({.name = "dodaylightcycle", .world_values = {{"level", "false"}}});

    const std::string platform_bytes = spark::proto_detail::buildPlatformStatistics(platform, statistics, &world);
    if (!check(spark::proto_test::hasVarint(platform_bytes, 3, 2000), "uptime was not encoded") ||
        !check(spark::proto_test::hasVarint(platform_bytes, 7, 4), "player count was not encoded") ||
        !check(spark::proto_test::hasVarint(platform_bytes, 9, 2), "online mode was not encoded") ||
        !check(spark::proto_test::findMessage(
                   platform_bytes, 8,
                   [](spark::ProtoReader message) { return spark::proto_test::hasVarint(message, 1, 8); }),
               "world statistics were not encoded")) {
        return 1;
    }

    const std::string world_bytes = spark::proto_detail::buildWorldStatistics(world);
    if (!check(spark::proto_test::findMessage(
                   world_bytes, 4,
                   [](spark::ProtoReader message) {
                       const spark::ProtoReader rule = message;
                       return spark::proto_test::hasString(rule, 1, "dodaylightcycle") &&
                              spark::proto_test::findMessage(rule, 3, [](spark::ProtoReader value) {
                                  const spark::ProtoReader world_value = value;
                                  return spark::proto_test::hasString(world_value, 1, "level") &&
                                         spark::proto_test::hasString(world_value, 2, "false");
                              });
                   }),
               "game rule statistics were not encoded")) {
        return 1;
    }

    spark::SystemStats system;
    system.cpu_present = true;
    system.cpu_threads = 8;
    system.cpu_model = "test-cpu";
    const std::string system_bytes = spark::proto_detail::buildSystemStatistics(system, statistics);
    if (!check(spark::proto_test::findMessage(system_bytes, 1,
                                              [](spark::ProtoReader cpu) {
                                                  return spark::proto_test::hasVarint(cpu, 1, 8) &&
                                                         spark::proto_test::hasString(cpu, 4, "test-cpu");
                                              }),
               "CPU statistics were not encoded") ||
        !check(spark::proto_test::hasMessage(system_bytes, 6), "native Java placeholder was not encoded")) {
        return 1;
    }

    spark::WindowStats window;
    window.ticks_present = true;
    window.ticks = 17;
    window.tile_entities_present = true;
    window.tile_entities = 5;
    window.duration_ms = 800;
    window.start_time_ms = 1000;
    window.end_time_ms = 1800;
    const std::string window_bytes = spark::proto_detail::buildWindowStatistics(window);
    if (!check(spark::proto_test::hasVarint(window_bytes, 1, 17), "window ticks were not encoded") ||
        !check(spark::proto_test::hasVarint(window_bytes, 9, 5), "window tile entities were not encoded") ||
        !check(spark::proto_test::hasVarint(window_bytes, 11, 1000), "window start was not encoded") ||
        !check(spark::proto_test::hasVarint(window_bytes, 13, 800), "window duration was not encoded")) {
        return 1;
    }

    spark::WindowStats zero_tile_entities;
    zero_tile_entities.tile_entities_present = true;
    const std::string zero_tile_bytes = spark::proto_detail::buildWindowStatistics(zero_tile_entities);
    if (!check(spark::proto_test::hasVarint(zero_tile_bytes, 9, 0), "real zero tile entities were omitted")) {
        return 1;
    }

    spark::WindowStats unavailable_tile_entities;
    unavailable_tile_entities.tile_entities = 9;
    unavailable_tile_entities.tile_entities_present = false;
    const std::string unavailable_tile_bytes = spark::proto_detail::buildWindowStatistics(unavailable_tile_entities);
    if (!check(!spark::proto_test::hasField(unavailable_tile_bytes, 9), "unavailable tile entities were encoded")) {
        return 1;
    }
    return 0;
}
