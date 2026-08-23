#include <cstdio>
#include <string>
#include <unordered_map>

#include "proto/sampler_data.h"
#include "proto_test_utils.h"

namespace {

bool check(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "sampler statistics: %s\n", message);
    }
    return value;
}

}  // namespace

int main()
{
    spark::ModuleTable modules;
    const spark::ModuleId module = modules.intern("statistics-module");
    const spark::FrameKey frame{.module = module, .rva = 0x10, .raw_address = 0x10};
    spark::CallTree tree;
    tree.log({frame}, 0);

    spark::ProfileMetadata metadata;
    metadata.start_time_ms = 1'000;
    metadata.end_time_ms = 1'800;
    metadata.platform_stats.present = true;
    metadata.system_stats.present = true;
    metadata.system_stats.cpu_threads = 8;

    metadata.statistics.tps.last_1m = {.present = true, .value = 19.0, .span_ms = 60'000, .samples = 1140};
    metadata.statistics.tps.last_5m = {.present = true, .value = 18.0, .span_ms = 300'000, .samples = 5400};
    metadata.statistics.tps.last_15m = {.present = true, .value = 17.0, .span_ms = 900'000, .samples = 15300};
    metadata.statistics.mspt.last_1m = {.present = true,
                                        .mean = 10.0,
                                        .min = 1.0,
                                        .median = 9.0,
                                        .percentile95 = 20.0,
                                        .max = 30.0,
                                        .span_ms = 60'000,
                                        .samples = 1140};
    metadata.statistics.mspt.last_5m = {.present = true,
                                        .mean = 11.0,
                                        .min = 2.0,
                                        .median = 10.0,
                                        .percentile95 = 22.0,
                                        .max = 35.0,
                                        .span_ms = 300'000,
                                        .samples = 5400};
    metadata.statistics.cpu.process_last_1m = {.present = true, .value = 0.25, .span_ms = 60'000, .samples = 60};
    metadata.statistics.cpu.process_last_15m = {.present = true, .value = 0.20, .span_ms = 900'000, .samples = 900};
    metadata.statistics.cpu.system_last_1m = {.present = true, .value = 0.50, .span_ms = 60'000, .samples = 60};
    metadata.statistics.cpu.system_last_15m = {.present = true, .value = 0.40, .span_ms = 900'000, .samples = 900};

    spark::WindowStats window;
    window.ticks_present = true;
    window.ticks = 17;
    window.cpu_process_present = true;
    window.cpu_process = 0.25;
    window.cpu_system_present = true;
    window.cpu_system = 0.50;
    window.tps_present = true;
    window.tps = 17.0;
    window.mspt_present = true;
    window.mspt_median = 9.0;
    window.mspt_max = 30.0;
    window.players_present = true;
    window.players = 4;
    window.start_time_ms = 1'000;
    window.end_time_ms = 1'800;
    window.duration_ms = 800;
    metadata.window_stats[0] = window;

    std::unordered_map<spark::FrameKey, spark::ResolvedFrame, spark::FrameKeyHash> resolved;
    resolved[frame] = {.class_name = "statistics", .method_name = "sample"};
    const std::string profile = spark::buildSamplerData(metadata, tree, resolved);

    const bool rolling_values = spark::proto_test::findMessageBytes(profile, 1, [](std::string_view metadata_bytes) {
        return spark::proto_test::findMessageBytes(
                   metadata_bytes, 8,
                   [](std::string_view platform_bytes) {
                       const bool tps =
                           spark::proto_test::findMessageBytes(platform_bytes, 4, [](std::string_view bytes) {
                               return spark::proto_test::hasDouble(bytes, 1, 19.0) &&
                                      spark::proto_test::hasDouble(bytes, 2, 18.0) &&
                                      spark::proto_test::hasDouble(bytes, 3, 17.0);
                           });
                       const bool mspt =
                           spark::proto_test::findMessageBytes(platform_bytes, 5, [](std::string_view bytes) {
                               return spark::proto_test::findMessageBytes(
                                          bytes, 1,
                                          [](std::string_view values) {
                                              return spark::proto_test::hasDouble(values, 1, 10.0) &&
                                                     spark::proto_test::hasDouble(values, 3, 1.0) &&
                                                     spark::proto_test::hasDouble(values, 4, 9.0) &&
                                                     spark::proto_test::hasDouble(values, 5, 20.0);
                                          }) &&
                                      spark::proto_test::findMessageBytes(bytes, 2, [](std::string_view values) {
                                          return spark::proto_test::hasDouble(values, 1, 11.0) &&
                                                 spark::proto_test::hasDouble(values, 3, 2.0) &&
                                                 spark::proto_test::hasDouble(values, 4, 10.0) &&
                                                 spark::proto_test::hasDouble(values, 5, 22.0);
                                      });
                           });
                       return tps && mspt;
                   }) &&
               spark::proto_test::findMessageBytes(metadata_bytes, 9, [](std::string_view system_bytes) {
                   return spark::proto_test::findMessageBytes(system_bytes, 1, [](std::string_view cpu_bytes) {
                       return spark::proto_test::findMessageBytes(
                                  cpu_bytes, 2,
                                  [](std::string_view usage) {
                                      return spark::proto_test::hasDouble(usage, 1, 0.25) &&
                                             spark::proto_test::hasDouble(usage, 2, 0.20);
                                  }) &&
                              spark::proto_test::findMessageBytes(cpu_bytes, 3, [](std::string_view usage) {
                                  return spark::proto_test::hasDouble(usage, 1, 0.50) &&
                                         spark::proto_test::hasDouble(usage, 2, 0.40);
                              });
                   });
               });
    });
    if (!check(rolling_values, "rolling TPS/MSPT/CPU values did not round-trip")) {
        return 1;
    }

    const bool window_values = spark::proto_test::findMessageBytes(profile, 7, [](std::string_view entry) {
        return spark::proto_test::hasVarint(entry, 1, 0) &&
               spark::proto_test::findMessageBytes(entry, 2, [](std::string_view statistics) {
                   return spark::proto_test::hasVarint(statistics, 1, 17) &&
                          spark::proto_test::hasDouble(statistics, 2, 0.25) &&
                          spark::proto_test::hasDouble(statistics, 3, 0.50) &&
                          spark::proto_test::hasDouble(statistics, 4, 17.0) &&
                          spark::proto_test::hasDouble(statistics, 5, 9.0) &&
                          spark::proto_test::hasDouble(statistics, 6, 30.0) &&
                          spark::proto_test::hasVarint(statistics, 7, 4) &&
                          spark::proto_test::hasVarint(statistics, 11, 1'000) &&
                          spark::proto_test::hasVarint(statistics, 12, 1'800) &&
                          spark::proto_test::hasVarint(statistics, 13, 800) &&
                          !spark::proto_test::hasField(statistics, 8) && !spark::proto_test::hasField(statistics, 10);
               });
    });
    if (!check(window_values, "window gauges or timestamps were incorrect")) {
        return 1;
    }

    const bool omitted_resources = spark::proto_test::findMessageBytes(profile, 1, [](std::string_view metadata_bytes) {
        const bool platform = spark::proto_test::findMessageBytes(
            metadata_bytes, 8, [](std::string_view bytes) { return !spark::proto_test::hasField(bytes, 1); });
        const bool system = spark::proto_test::findMessageBytes(metadata_bytes, 9, [](std::string_view bytes) {
            return !spark::proto_test::hasField(bytes, 2) && !spark::proto_test::hasField(bytes, 4) &&
                   !spark::proto_test::hasField(bytes, 5);
        });
        return platform && system;
    });
    if (!check(omitted_resources, "unavailable resource fields were serialized")) {
        return 1;
    }

    spark::ProfileMetadata game_rule_metadata;
    game_rule_metadata.platform_stats.present = true;
    game_rule_metadata.world.present = true;
    game_rule_metadata.world.game_rules.push_back({.name = "dodaylightcycle", .world_values = {{"level", "false"}}});
    const std::string game_rule_profile = spark::buildSamplerData(game_rule_metadata, tree, {});
    const bool game_rule_values =
        spark::proto_test::findMessageBytes(game_rule_profile, 1, [](std::string_view metadata_bytes) {
            return spark::proto_test::findMessageBytes(metadata_bytes, 8, [](std::string_view platform_bytes) {
                return spark::proto_test::findMessageBytes(platform_bytes, 8, [](std::string_view world_bytes) {
                    return spark::proto_test::findMessageBytes(world_bytes, 4, [](std::string_view rule) {
                        return spark::proto_test::hasString(rule, 1, "dodaylightcycle") &&
                               !spark::proto_test::hasField(rule, 2) &&
                               spark::proto_test::findMessageBytes(rule, 3, [](std::string_view world_value) {
                                   return spark::proto_test::hasString(world_value, 1, "level") &&
                                          spark::proto_test::hasString(world_value, 2, "false");
                               });
                    });
                });
            });
        });
    if (!check(game_rule_values, "game-rule name/current mapping or default omission was incorrect")) {
        return 1;
    }
    return 0;
}
