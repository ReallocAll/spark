#ifndef ENDSTONE_SPARK_METRICS_PROTO_H
#define ENDSTONE_SPARK_METRICS_PROTO_H

#include <string>

#include "core/stats/metrics_history.h"

namespace spark::proto_detail {

// Encode the optional spark Metrics message. Empty series are omitted.
std::string buildMetrics(const MetricsSnapshot &metrics);

}  // namespace spark::proto_detail

#endif  // ENDSTONE_SPARK_METRICS_PROTO_H
