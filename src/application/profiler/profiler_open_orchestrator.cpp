#include "application/profiler/profiler_open_orchestrator.h"

#include <string>
#include <utility>

#include "core/stats/system_stats.h"
#include "core/util/base64.h"
#include "core/util/format.h"
#include "core/util/monotonic_time.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

}  // namespace

ProfilerOpenOrchestrator::ProfilerOpenOrchestrator(Profiler &profiler, StatisticsService &statistics,
                                                   std::string bds_executable_sha256, std::string bytebin_url,
                                                   std::string viewer_url, std::string bytesocks_host,
                                                   TrustedViewersState &trusted_viewers,
                                                   MainThreadDispatcher &dispatcher,
                                                   ProfileMetadataProvider &metadata_provider, ResultNotifier &notifier)
    : profiler_(profiler), statistics_(statistics), bds_executable_sha256_(std::move(bds_executable_sha256)),
      bytebin_url_(std::move(bytebin_url)), viewer_url_(std::move(viewer_url)),
      bytesocks_host_(std::move(bytesocks_host)), trusted_viewers_(trusted_viewers), dispatcher_(dispatcher),
      metadata_provider_(metadata_provider), notifier_(notifier)
{
    viewer_open_fn_ = [](ViewerSocket &socket, const ViewerSocket::UploadCallback &upload) {
        return socket.open(upload);
    };
    viewer_worker_ = std::make_unique<ViewerUpdateWorker>(
        [this](const ViewerUpdateWorker::WorkItem &work) { return executeViewerWork(work); },
        [this](ViewerUpdateWorker::Completion completion) { completeViewerWork(std::move(completion)); });
}

ProfilerOpenOrchestrator::~ProfilerOpenOrchestrator()
{
    shutdown();
}

void ProfilerOpenOrchestrator::cmdOpen(CommandSender &sender, const Arguments &args)
{
    if (viewerOpenPending()) {
        sender.sendMessage("A live viewer is already being opened.");
        return;
    }
    if (viewer_socket_ && viewer_socket_->isOpen()) {
        sender.sendMessage("A live viewer is already open.");
        return;
    }
    if (!profiler_.running()) {
        sender.sendMessage("The profiler isn't running! Start it first with: {}/spark profiler start", kColorGray);
        return;
    }
    auto key_pair = Crypto::generateKeyPair();
    if (key_pair.public_key_x509.empty()) {
        sender.sendErrorMessage("Failed to generate cryptographic key pair for the live viewer.");
        return;
    }

    ViewerSocket::Config config;
    config.bytesocks_host = bytesocks_host_;
    config.bytebin_url = bytebin_url_;
    config.viewer_url = viewer_url_;
    config.user_agent = std::string("endstone-spark/") + kVersion;

    auto socket = std::make_shared<ViewerSocket>(std::move(config), std::move(key_pair));
    socket->setIsKeyTrustedCallback([this](const std::vector<std::uint8_t> &key) {
        std::string b64 = base64Encode(key.data(), key.size());
        return trusted_viewers_.contains(b64);
    });

    std::string comment;
    const auto comments = args.stringFlag("comment");
    if (!comments.empty()) {
        comment = comments.front();
    }
    ExportContext context = captureLiveContext(nowMs(), comment);
    if (!startViewerWorker()) {
        sender.sendErrorMessage("Failed to start the live viewer worker.");
        return;
    }
    open_comment_ = comment;
    if (!viewer_worker_->enqueueOpen(std::move(context), std::move(socket), sender.getName())) {
        open_comment_.clear();
        sender.sendErrorMessage("Failed to start the live viewer worker.");
        return;
    }
    sender.sendMessage("{}Opening the live viewer...{}", kColorGold, kColorGray);
}

void ProfilerOpenOrchestrator::onTick(const std::string &fallback_sender_name)
{
    if (viewer_worker_ && viewer_worker_->consumeFailure()) {
        notifier_.notify(viewer_sender_name_.empty() ? fallback_sender_name : viewer_sender_name_,
                         "Live viewer worker failed.");
        close();
    }
    if (!profiler_.running()) {
        return;
    }

    if (viewer_socket_) {
        if (!viewer_socket_->tick()) {
            std::string diagnostic = viewer_socket_->takeDiagnostic();
            if (!diagnostic.empty()) {
                notifier_.notify(viewer_sender_name_.empty() ? fallback_sender_name : viewer_sender_name_, diagnostic);
            }
            close();
        }
        else if (viewer_socket_->isOpen()) {
            const auto now = nowMs();
            if (now - last_viewer_upload_ms_ >= 10000) {
                const bool available = viewer_worker_ && viewer_worker_->available();
                const std::uint64_t generation = viewer_worker_ ? viewer_worker_->generation() : 0;
                if (available) {
                    ExportContext context = captureLiveContext(now, open_comment_);
                    if (viewer_worker_->enqueueUpdate(std::move(context), viewer_socket_, generation)) {
                        last_viewer_upload_ms_ = now;
                    }
                }
            }
        }
    }
}

void ProfilerOpenOrchestrator::close()
{
    if (viewer_worker_) {
        viewer_worker_->invalidate();
    }
    std::shared_ptr<ViewerSocket> socket = std::move(viewer_socket_);
    if (socket) {
        socket->close();
    }
    last_viewer_upload_ms_ = 0;
    viewer_sender_name_.clear();
    open_comment_.clear();
}

void ProfilerOpenOrchestrator::shutdown()
{
    close();
    stopViewerWorker();
    lifetime_.reset();
}

void ProfilerOpenOrchestrator::setViewerSocketForTesting(std::shared_ptr<ViewerSocket> socket)
{
    viewer_socket_ = std::move(socket);
    viewer_sender_name_ = "Console";
}

bool ProfilerOpenOrchestrator::startViewerWorker()
{
    return viewer_worker_ && viewer_worker_->start();
}

void ProfilerOpenOrchestrator::stopViewerWorker()
{
    if (viewer_worker_) {
        viewer_worker_->stop();
    }
}

std::string ProfilerOpenOrchestrator::executeViewerWork(const ViewerUpdateWorker::WorkItem &work)
{
    if (!viewerGenerationCurrent(work.generation) || !profiler_.running()) {
        return {};
    }

    if (work.type == ViewerUpdateWorker::WorkType::Open) {
        ExportContext context = work.context;
        return viewer_open_fn_(*work.socket,
                               [this, &context, generation = work.generation](const std::string &channel_info_proto) {
                                   if (!viewerGenerationCurrent(generation) || !profiler_.running()) {
                                       return std::string();
                                   }
                                   context.socket_channel_info_proto = channel_info_proto;
                                   return uploadSamplerData(context);
                               });
    }

    std::string bytebin_key = uploadSamplerData(work.context);
    if (!bytebin_key.empty() && viewerGenerationCurrent(work.generation)) {
        work.socket->sendUpdate(bytebin_key);
    }
    return {};
}

void ProfilerOpenOrchestrator::completeViewerWork(ViewerUpdateWorker::Completion completion) noexcept
{
    const std::weak_ptr<int> lifetime = lifetime_;
    if (lifetime.expired()) {
        return;
    }
    const std::uint64_t generation = completion.generation;
    try {
        dispatcher_.runOnMainThread([this, lifetime, completion = std::move(completion)]() mutable {
            if (lifetime.expired()) {
                return;
            }
            completeViewerOpen(std::move(completion));
        });
    }
    catch (...) {
        if (!lifetime.expired() && viewer_worker_) {
            try {
                viewer_worker_->completeOpen(generation);
            }
            catch (...) {
                return;
            }
        }
    }
}

void ProfilerOpenOrchestrator::completeViewerOpen(ViewerUpdateWorker::Completion completion)
{
    if (!viewer_worker_ || !viewer_worker_->completeOpen(completion.generation)) {
        return;
    }
    if (completion.url.empty() || !completion.socket || !completion.socket->isOpen() || !profiler_.running()) {
        if (completion.socket) {
            completion.socket->close();
        }
        open_comment_.clear();
        notifier_.notify(completion.sender_name, "Failed to open the live viewer. Check your network connection.");
        return;
    }
    viewer_socket_ = std::move(completion.socket);
    viewer_sender_name_ = completion.sender_name;
    last_viewer_upload_ms_ = nowMs();
    notifier_.notify(completion.sender_name, "Live viewer opened! Open it at: " + completion.url);
    notifier_.notify(completion.sender_name, "The viewer updates every 10 seconds while the profiler is running.");
}

ExportContext ProfilerOpenOrchestrator::captureLiveContext(std::int64_t now_ms, const std::string &comment)
{
    ExportContext context;
    context.bds_executable_sha256 = bds_executable_sha256_;
    metadata_provider_.gatherServerMetadata(context, now_ms);
    if (native_plugin_sources_provider_) {
        context.native_plugin_sources = native_plugin_sources_provider_();
    }
    context.comment = comment;
    context.statistics = statistics_.snapshot();
    context.metrics = statistics_.metricsSnapshot();
    context.window_stats = statistics_.profileWindows(profiler_.startTimeMs(), now_ms);
    context.system_stats = spark::gatherSystemStats(".");
    metadata_provider_.gatherWorldMetadata(context);
    if (ping_samples_provider_) {
        context.ping_samples = ping_samples_provider_();
    }
    if (network_snapshot_provider_) {
        context.net_snapshots = network_snapshot_provider_();
    }
    return context;
}

std::string ProfilerOpenOrchestrator::uploadSamplerData(const ExportContext &context)
{
    const bool tracking_was_suppressed = profiler_.setCurrentThreadAllocationTrackingSuppressed(true);
    try {
        std::string body = buildLiveSamplerData(context);
        if (body.empty()) {
            profiler_.setCurrentThreadAllocationTrackingSuppressed(tracking_was_suppressed);
            return {};
        }
        std::string compressed = gzipCompress(body);
        UploadResult result =
            uploadToBytebin(compressed, bytebin_url_, kSamplerContentType, std::string("endstone-spark/") + kVersion);
        profiler_.setCurrentThreadAllocationTrackingSuppressed(tracking_was_suppressed);
        return result.ok ? result.key : std::string();
    }
    catch (...) {
        profiler_.setCurrentThreadAllocationTrackingSuppressed(tracking_was_suppressed);
        throw;
    }
}

std::string ProfilerOpenOrchestrator::buildLiveSamplerData(const ExportContext &context)
{
    return profiler_.liveExport(context);
}

bool ProfilerOpenOrchestrator::viewerGenerationCurrent(std::uint64_t generation) const
{
    return viewer_worker_ && viewer_worker_->current(generation);
}

}  // namespace spark
