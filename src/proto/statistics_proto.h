#ifndef ENDSTONE_SPARK_STATISTICS_PROTO_H
#define ENDSTONE_SPARK_STATISTICS_PROTO_H

#include <string>

#include "core/stats/statistics_service.h"
#include "core/stats/system_stats.h"

namespace spark::proto_detail {

// Encode the spark PlatformStatistics message. `world` is optional because
// health reports do not carry world statistics.
std::string buildPlatformStatistics(const PlatformStats &platform, const StatisticsSnapshot &statistics,
                                    const WorldInfo *world = nullptr);

// Encode the spark SystemStatistics message.
std::string buildSystemStatistics(const SystemStats &system, const StatisticsSnapshot &statistics);

// Encode the spark WorldStatistics message.
std::string buildWorldStatistics(const WorldInfo &world);

// Encode one spark WindowStatistics message.
std::string buildWindowStatistics(const WindowStats &window);

}  // namespace spark::proto_detail

#endif  // ENDSTONE_SPARK_STATISTICS_PROTO_H
