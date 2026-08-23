#ifndef SPARK_APPLICATION_HEALTH_HEALTH_REPORT_H
#define SPARK_APPLICATION_HEALTH_HEALTH_REPORT_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "core/stats/network_monitor.h"
#include "core/stats/statistics_service.h"
#include "proto/health_data.h"

namespace spark {

void sendPerformanceReport(CommandSender &sender, const StatisticsSnapshot &stats);

void showHealthReport(CommandSender &sender, StatisticsService &statistics, ProfileMetadataProvider &metadata_provider,
                      const std::map<std::string, NetworkInterfaceSnapshot> &network_snapshots);

HealthData captureHealthData(StatisticsService &statistics, ProfileMetadataProvider &metadata_provider,
                             const std::string &sender_name, bool sender_is_player, std::int64_t now_ms,
                             const std::vector<int> &ping_samples,
                             const std::map<std::string, NetworkInterfaceSnapshot> &network_snapshots);

}  // namespace spark

#endif  // SPARK_APPLICATION_HEALTH_HEALTH_REPORT_H
