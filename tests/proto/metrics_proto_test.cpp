#include <cassert>
#include <string>
#include <unordered_map>

#include "proto/health_data.h"
#include "proto/metrics_proto.h"
#include "proto/sampler_data.h"
#include "proto_test_utils.h"

namespace {

spark::MetricsSnapshot sampleMetrics()
{
    spark::MetricsSnapshot metrics;
    metrics.tps = {{.timestamp_ms = 1'000, .value = 20.0}, {.timestamp_ms = 11'001, .value = 19.5}};
    metrics.tick_duration = {{.timestamp_ms = 1'000,
                              .values = {.mean = 10.0, .max = 20.0, .min = 1.0, .median = 9.0, .percentile95 = 19.0}}};
    metrics.cpu_usage_process = {{.timestamp_ms = 1'000, .value = 0.25}};
    metrics.cpu_usage_system = {{.timestamp_ms = 1'000, .value = 0.50}};
    metrics.world_info = {{.timestamp_ms = 1'000, .players = 4, .entities = 30, .chunks = 40}};
    metrics.player_ping = {{.timestamp_ms = 1'000,
                            .values = {.mean = 30.0, .max = 50.0, .min = 10.0, .median = 30.0, .percentile95 = 50.0}}};
    return metrics;
}

void testMetricsFields()
{
    const std::string bytes = spark::proto_detail::buildMetrics(sampleMetrics());
    assert(spark::proto_test::hasMessage(bytes, 1));
    assert(spark::proto_test::hasMessage(bytes, 2));
    assert(spark::proto_test::hasMessage(bytes, 3));
    assert(spark::proto_test::hasMessage(bytes, 4));
    assert(!spark::proto_test::hasField(bytes, 5));
    assert(!spark::proto_test::hasField(bytes, 6));
    assert(!spark::proto_test::hasField(bytes, 7));
    assert(spark::proto_test::hasMessage(bytes, 8));
    assert(spark::proto_test::hasMessage(bytes, 9));
    assert(spark::proto_test::findMessageBytes(bytes, 1, [](std::string_view series) {
        return spark::proto_test::hasVarint(series, 1, 1'000) && spark::proto_test::hasField(series, 2) &&
               spark::proto_test::hasField(series, 3);
    }));
    assert(spark::proto_test::findMessageBytes(bytes, 8, [](std::string_view series) {
        return spark::proto_test::findMessageBytes(series, 3, [](std::string_view values) {
            return spark::proto_test::hasVarint(values, 1, 4) && spark::proto_test::hasVarint(values, 2, 30) &&
                   spark::proto_test::hasVarint(values, 4, 40) && !spark::proto_test::hasField(values, 3);
        });
    }));
}

void testMetadataFields()
{
    const spark::MetricsSnapshot metrics = sampleMetrics();
    spark::HealthData health;
    health.metrics = metrics;
    const std::string health_bytes = spark::buildHealthData(health);
    assert(spark::proto_test::findMessageBytes(health_bytes, 1, [](std::string_view metadata) {
        return spark::proto_test::findMessageBytes(metadata, 9, [](std::string_view bytes) {
            return spark::proto_test::hasMessage(bytes, 1) && spark::proto_test::hasMessage(bytes, 9);
        });
    }));

    spark::ModuleTable modules;
    const spark::ModuleId module = modules.intern("metrics");
    const spark::FrameKey frame{.module = module, .rva = 1, .raw_address = 1};
    spark::CallTree tree;
    tree.log({frame}, 0);
    spark::ProfileMetadata profile;
    profile.metrics = metrics;
    const std::string profile_bytes = spark::buildSamplerData(profile, tree, {});
    assert(spark::proto_test::findMessageBytes(profile_bytes, 1, [](std::string_view metadata) {
        return spark::proto_test::findMessageBytes(metadata, 18, [](std::string_view bytes) {
            return spark::proto_test::hasMessage(bytes, 1) && spark::proto_test::hasMessage(bytes, 9);
        });
    }));
}

}  // namespace

int main()
{
    testMetricsFields();
    testMetadataFields();
    return 0;
}
