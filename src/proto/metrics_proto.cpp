#include "proto/metrics_proto.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "proto/proto_writer.h"

namespace spark::proto_detail {
namespace {

std::vector<std::uint32_t> timestampDeltas(const auto &samples)
{
    std::vector<std::uint32_t> deltas;
    deltas.reserve(samples.size());
    std::int64_t previous = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::int64_t timestamp = samples[i].timestamp_ms;
        if (i == 0) {
            deltas.push_back(0);
        }
        else {
            const std::int64_t delta = timestamp - previous;
            if (delta < 0 || delta > std::numeric_limits<std::uint32_t>::max()) {
                throw std::logic_error("metric timestamp delta cannot be represented as uint32");
            }
            deltas.push_back(static_cast<std::uint32_t>(delta));
        }
        previous = timestamp;
    }
    return deltas;
}

template <typename Samples>
void writeSeriesHeader(ProtoWriter &writer, const Samples &samples)
{
    if (samples.empty()) {
        return;
    }
    if (samples.front().timestamp_ms != 0) {
        writer.int64(1, samples.front().timestamp_ms);
    }
    writer.packedUint32(2, timestampDeltas(samples));
}

void writeAverages(ProtoWriter &writer, const MetricsAverages &values)
{
    if (values.mean != 0.0) {
        writer.real(1, values.mean);
    }
    if (values.max != 0.0) {
        writer.real(2, values.max);
    }
    if (values.min != 0.0) {
        writer.real(3, values.min);
    }
    if (values.median != 0.0) {
        writer.real(4, values.median);
    }
    if (values.percentile95 != 0.0) {
        writer.real(5, values.percentile95);
    }
}

std::string buildDoubleSeries(const std::vector<MetricsDoubleSample> &samples)
{
    std::string out;
    ProtoWriter writer(out);
    writeSeriesHeader(writer, samples);
    std::vector<double> values;
    values.reserve(samples.size());
    for (const MetricsDoubleSample &sample : samples) {
        values.push_back(sample.value);
    }
    writer.packedDouble(3, values);
    return out;
}

std::string buildAveragesSeries(const std::vector<MetricsAveragesSample> &samples)
{
    std::string out;
    ProtoWriter writer(out);
    writeSeriesHeader(writer, samples);
    for (const MetricsAveragesSample &sample : samples) {
        std::string values;
        ProtoWriter values_writer(values);
        writeAverages(values_writer, sample.values);
        writer.message(3, values);
    }
    return out;
}

std::string buildWorldSeries(const std::vector<MetricsWorldInfoSample> &samples)
{
    std::string out;
    ProtoWriter writer(out);
    writeSeriesHeader(writer, samples);
    for (const MetricsWorldInfoSample &sample : samples) {
        std::string values;
        ProtoWriter values_writer(values);
        if (sample.players != 0) {
            values_writer.int32(1, sample.players);
        }
        if (sample.entities != 0) {
            values_writer.int32(2, sample.entities);
        }
        if (sample.tile_entities != 0) {
            values_writer.int32(3, sample.tile_entities);
        }
        if (sample.chunks != 0) {
            values_writer.int32(4, sample.chunks);
        }
        writer.message(3, values);
    }
    return out;
}

}  // namespace

std::string buildMetrics(const MetricsSnapshot &metrics)
{
    std::string out;
    ProtoWriter writer(out);
    if (!metrics.tps.empty()) {
        writer.message(1, buildDoubleSeries(metrics.tps));
    }
    if (!metrics.tick_duration.empty()) {
        writer.message(2, buildAveragesSeries(metrics.tick_duration));
    }
    if (!metrics.cpu_usage_process.empty()) {
        writer.message(3, buildDoubleSeries(metrics.cpu_usage_process));
    }
    if (!metrics.cpu_usage_system.empty()) {
        writer.message(4, buildDoubleSeries(metrics.cpu_usage_system));
    }
    if (!metrics.world_info.empty()) {
        writer.message(8, buildWorldSeries(metrics.world_info));
    }
    if (!metrics.player_ping.empty()) {
        writer.message(9, buildAveragesSeries(metrics.player_ping));
    }
    return out;
}

}  // namespace spark::proto_detail
