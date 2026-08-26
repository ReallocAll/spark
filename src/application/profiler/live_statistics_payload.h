#ifndef SPARK_APPLICATION_PROFILER_LIVE_STATISTICS_PAYLOAD_H
#define SPARK_APPLICATION_PROFILER_LIVE_STATISTICS_PAYLOAD_H

#include <string>

#include "core/profiler/profiler.h"

namespace spark {

struct LiveStatisticsPayload {
    std::string platform;
    std::string system;
    std::string metrics;
};

// Builds the three serialized statistics messages sent by the live viewer.
LiveStatisticsPayload buildLiveStatisticsPayload(const ExportContext &context);

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_LIVE_STATISTICS_PAYLOAD_H
