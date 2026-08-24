#ifndef SPARK_APPLICATION_PROFILER_PROFILER_OPEN_ORCHESTRATOR_H
#define SPARK_APPLICATION_PROFILER_PROFILER_OPEN_ORCHESTRATOR_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "application/profiler/live_viewer_schedule.h"
#include "application/profiler/viewer_update_worker.h"
#include "core/command/arguments.h"
#include "core/config/trusted_viewers.h"
#include "core/profiler/profiler.h"
#include "core/stats/network_monitor.h"
#include "core/stats/statistics_service.h"
#include "core/ws/viewer_socket.h"

namespace spark {

class ProfilerService;
struct ProfilerOpenTestAccess;

// Owns live-viewer open, export, and update orchestration for a running profile.
class ProfilerOpenOrchestrator {
public:
    ProfilerOpenOrchestrator(Profiler &profiler, StatisticsService &statistics, std::string bds_executable_sha256,
                             std::string bytebin_url, std::string viewer_url, std::string bytesocks_host,
                             TrustedViewersState &trusted_viewers, MainThreadDispatcher &dispatcher,
                             ProfileMetadataProvider &metadata_provider, ResultNotifier &notifier);
    ~ProfilerOpenOrchestrator();

    ProfilerOpenOrchestrator(const ProfilerOpenOrchestrator &) = delete;
    ProfilerOpenOrchestrator &operator=(const ProfilerOpenOrchestrator &) = delete;

    void cmdOpen(CommandSender &sender, const Arguments &args);
    void onTick(const std::string &fallback_sender_name);
    void close();
    void shutdown();

    void setNativePluginSourcesProvider(std::function<std::vector<NativePluginSource>()> provider)
    {
        native_plugin_sources_provider_ = std::move(provider);
    }
    void setPingSamplesProvider(std::function<std::vector<int>()> provider)
    {
        ping_samples_provider_ = std::move(provider);
    }
    void setNetworkSnapshotProvider(std::function<std::map<std::string, NetworkInterfaceSnapshot>()> provider)
    {
        network_snapshot_provider_ = std::move(provider);
    }

    std::shared_ptr<ViewerSocket> viewerSocket() const { return viewer_socket_; }

private:
    friend class ProfilerService;
    friend struct ProfilerOpenTestAccess;

    bool viewerOpenPending() const { return viewer_worker_ && viewer_worker_->openPending(); }
    bool hasViewerSocket() const { return viewer_socket_ != nullptr; }
    std::shared_ptr<ViewerSocket> viewerSocketForTesting() const { return viewer_socket_; }
    const std::string &openCommentForTesting() const { return open_comment_; }
    void setViewerSocketForTesting(std::shared_ptr<ViewerSocket> socket);
    void setViewerOpenFunctionForTesting(
        std::function<std::string(ViewerSocket &, const ViewerSocket::UploadCallback &)> open_function)
    {
        viewer_open_fn_ = std::move(open_function);
    }

    ExportContext captureLiveContext(std::int64_t now_ms, const std::string &comment = {});
    ExportContext captureLiveStatisticsContext(std::int64_t now_ms);
    std::string buildLiveSamplerData(const ExportContext &context);
    std::string buildLiveSamplerData(std::int64_t now_ms) { return buildLiveSamplerData(captureLiveContext(now_ms)); }
    std::string uploadSamplerData(const ExportContext &context);
    bool viewerGenerationCurrent(std::uint64_t generation) const;
    bool startViewerWorker();
    void stopViewerWorker();
    std::string executeViewerWork(const ViewerUpdateWorker::WorkItem &work);
    void completeViewerWork(ViewerUpdateWorker::Completion completion) noexcept;
    void completeViewerOpen(ViewerUpdateWorker::Completion completion);

    Profiler &profiler_;
    StatisticsService &statistics_;
    std::string bds_executable_sha256_;
    std::string bytebin_url_;
    std::string viewer_url_;
    std::string bytesocks_host_;
    TrustedViewersState &trusted_viewers_;
    MainThreadDispatcher &dispatcher_;
    ProfileMetadataProvider &metadata_provider_;
    ResultNotifier &notifier_;

    std::function<std::vector<NativePluginSource>()> native_plugin_sources_provider_;
    std::function<std::vector<int>()> ping_samples_provider_;
    std::function<std::map<std::string, NetworkInterfaceSnapshot>()> network_snapshot_provider_;

    std::shared_ptr<ViewerSocket> viewer_socket_;
    LiveViewerSchedule viewer_schedule_;
    std::string viewer_sender_name_;
    std::string open_comment_;

    std::unique_ptr<ViewerUpdateWorker> viewer_worker_;
    std::function<std::string(ViewerSocket &, const ViewerSocket::UploadCallback &)> viewer_open_fn_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILER_OPEN_ORCHESTRATOR_H
