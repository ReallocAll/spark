#ifndef ENDSTONE_SPARK_STATISTICS_PROTO_H
#define ENDSTONE_SPARK_STATISTICS_PROTO_H

#include <string>

#include "core/stats/process_memory.h"
#include "core/stats/statistics_service.h"
#include "core/stats/system_stats.h"

namespace spark::proto_detail {

// Encode the spark PlatformStatistics message. `world` is optional because
// health reports do not carry world statistics. `process_memory_override` is
// used by deterministic protocol tests; production callers gather a fresh
// native process-memory snapshot while serializing.
std::string buildPlatformStatistics(const PlatformStats &platform, const StatisticsSnapshot &statistics,
                                    const WorldInfo *world = nullptr,
                                    const ProcessMemoryUsage *process_memory_override = nullptr);

// Encode the spark SystemStatistics message.
std::string buildSystemStatistics(const SystemStats &system, const StatisticsSnapshot &statistics);

// Encode the spark WorldStatistics message.
std::string buildWorldStatistics(const WorldInfo &world);

// Encode one spark WindowStatistics message.
std::string buildWindowStatistics(const WindowStats &window);

}  // namespace spark::proto_detail

#endif  // ENDSTONE_SPARK_STATISTICS_PROTO_H
