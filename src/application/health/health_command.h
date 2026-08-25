#ifndef SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H
#define SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "application/command/command_sender.h"
#include "application/health/health_dashboard.h"
#include "application/platform_capabilities.h"
#include "core/activity/activity_log.h"
#include "core/command/arguments.h"
#include "core/config/trusted_viewers.h"
#include "core/stats/network_monitor.h"
#include "core/stats/ping_statistics.h"
#include "core/stats/statistics_service.h"
#include "net/bytebin.h"
#include "net/cancellation.h"
#include "proto/sampler_data.h"

namespace spark {

struct HealthCommandTestAccess;

// Handles /spark tps, /spark ping, and /spark health commands.
// Platform-independent: uses CommandSender and ProfileMetadataProvider.
class HealthCommand {
public:
    using UploadFunction = std::function<UploadResult(const std::string &, const std::string &, const std::string &,
                                                      const std::string &, CancellationToken)>;

    HealthCommand(StatisticsService &statistics, ProfileMetadataProvider &metadata_provider, std::string bytebin_url,
                  std::string viewer_url, std::string bytesocks_host, TrustedViewersState &trusted_viewers,
                  MainThreadDispatcher &dispatcher, ResultNotifier &notifier,
                  HealthDashboard::ConnectionFactory dashboard_factory = {}, UploadFunction upload_function = {});
    ~HealthCommand();

    void cmdTps(CommandSender &sender);
    void cmdPing(CommandSender &sender, const Arguments &args);
    void cmdHealth(CommandSender &sender, const Arguments &args);
    void onTick();

    // Called periodically (every ~10 seconds) to poll ping data.
    void pollPing();

    // Called periodically (every ~60 seconds) to poll network interface stats.
    void pollNetwork();

    // Returns the current ping samples for profile export, or an empty vector
    // if ping monitoring is not active.
    std::vector<int> pingSamples() const;

    // Returns the current network interface snapshots for profile export.
    std::map<std::string, NetworkInterfaceSnapshot> networkSnapshots() const;
    bool shutdownWithin(std::chrono::milliseconds timeout);
    void shutdown();

    // Sets a callback that returns the activity log, or nullptr if not available.
    void setActivityLogProvider(std::function<ActivityLog *()> provider)
    {
        activity_log_provider_ = std::move(provider);
    }

private:
    friend struct HealthCommandTestAccess;

    void showHealth(CommandSender &sender, const Arguments &args);
    void openHealthDashboard(CommandSender &sender);
    void trustViewer(CommandSender &sender, const Arguments &args);
    void uploadHealthReport(CommandSender &sender);
    HealthData captureHealthData(const CommandSender &sender, std::int64_t now_ms);
    HealthData captureHealthDataForSender(const std::string &sender_name, bool sender_is_player, std::int64_t now_ms);
    void onTickAt(std::int64_t now_ms);
    UploadResult uploadHealthData(const HealthData &data, CancellationToken cancellation = {});
    void runHealthUpload(const HealthData &data, std::string sender_name, bool sender_is_player, std::int64_t now_ms,
                         CancellationToken cancellation);
    void signalUploadWorkerExit() noexcept;
    void notifyBestEffort(const std::string &sender_name, const std::string &message) noexcept;
    void announceHealthUpload() noexcept;
    void completeHealthDashboard(HealthDashboard::OpenResult result);

    StatisticsService &statistics_;
    ProfileMetadataProvider &metadata_provider_;
    TrustedViewersState &trusted_viewers_;
    MainThreadDispatcher &dispatcher_;
    ResultNotifier &notifier_;
    std::unique_ptr<PingStatistics> ping_statistics_;
    NetworkMonitor network_monitor_;
    std::function<ActivityLog *()> activity_log_provider_;
    std::string bytebin_url_;
    std::string viewer_url_;
    std::string bytesocks_host_;
    std::unique_ptr<HealthDashboard> dashboard_;
    std::string dashboard_sender_;
    bool dashboard_sender_is_player_ = false;
    std::int64_t dashboard_open_time_ms_ = 0;
    std::uint64_t accepted_dashboard_generation_ = 0;
    std::thread upload_thread_;
    std::atomic<bool> uploading_{false};
    std::atomic<bool> stopping_{false};
    CancellationSource upload_cancellation_;
    std::mutex upload_exit_mutex_;
    std::condition_variable upload_exit_cv_;
    bool upload_worker_exited_ = true;
    std::mutex upload_mutex_;
    UploadResult upload_result_;
    std::string upload_sender_;
    bool upload_sender_is_player_ = false;
    std::int64_t upload_time_ms_ = 0;
    UploadFunction upload_fn_;
    std::atomic<std::shared_ptr<int>> lifetime_{std::make_shared<int>(0)};

    static constexpr auto kDefaultShutdownBudget = std::chrono::seconds(2);
};

}  // namespace spark

#endif  // SPARK_APPLICATION_HEALTH_HEALTH_COMMAND_H
