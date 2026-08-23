#include "application/profiler/profiler_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "application/profiler/profiler_start_options.h"
#include "core/stats/system_stats.h"
#include "core/util/base64.h"
#include "core/util/format.h"
#include "core/ws/crypto.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ProfilerService::ProfilerService(StatisticsService &statistics, std::string bds_executable_sha256,
                                 std::filesystem::path profile_storage_dir, std::string bytebin_url,
                                 std::string viewer_url, std::string bytesocks_host, bool background_enabled,
                                 int background_interval, std::string background_thread_grouper,
                                 std::string background_thread_dumper, TrustedViewersState &trusted_viewers,
                                 MainThreadDispatcher &dispatcher, ProfileMetadataProvider &metadata_provider,
                                 ResultNotifier &notifier)
    : statistics_(statistics), bds_executable_sha256_(std::move(bds_executable_sha256)), dispatcher_(dispatcher),
      metadata_provider_(metadata_provider), notifier_(notifier),
      exporter_(std::move(profile_storage_dir), bytebin_url, viewer_url), background_enabled_(background_enabled),
      background_interval_(background_interval), background_thread_grouper_(std::move(background_thread_grouper)),
      background_thread_dumper_(std::move(background_thread_dumper)), bytebin_url_(std::move(bytebin_url)),
      viewer_url_(std::move(viewer_url)), bytesocks_host_(std::move(bytesocks_host)), trusted_viewers_(trusted_viewers)
{
    viewer_open_fn_ = [](ViewerSocket &socket, const ViewerSocket::UploadCallback &upload) {
        return socket.open(upload);
    };
    viewer_worker_ = std::make_unique<ViewerUpdateWorker>(
        [this](const ViewerUpdateWorker::WorkItem &work) { return executeViewerWork(work); },
        [this](ViewerUpdateWorker::Completion completion) { completeViewerWork(std::move(completion)); });
}

ProfilerService::~ProfilerService()
{
    shutdown();
}

void ProfilerService::shutdown()
{
    profiler_.requestStop();
    lifetime_.reset();
    closeViewerSocket();
    stopViewerWorker();
    if (export_thread_.joinable()) {
        export_thread_.join();
    }
    export_completion_pending_.store(false);
    exporting_.store(false);
    restart_background_after_export_ = false;
}

void ProfilerService::cmdStart(CommandSender &sender, const Arguments &args)
{
    if (exporting_.load()) {
        sender.sendMessage("The profiler has stopped; results are still being finalized.");
        return;
    }

    auto parsed = spark::parseProfilerStartOptions(args);
    if (!parsed.success()) {
        sender.sendErrorMessage(parsed.error);
        return;
    }
    spark::ProfilerOptions options = std::move(parsed.options);
    options.creator_name = sender.getName();
    options.creator_is_player = sender.isPlayer();
    const std::int64_t timeout = options.timeout_seconds;

    std::uint64_t tid = main_tid_;
    if (tid == 0) {
        sender.sendErrorMessage("The server thread hasn't been identified yet - try again in a moment.");
        return;
    }

    if (profiler_.running()) {
        if (session_type_ != SessionType::Background) {
            cmdInfo(sender);
            return;
        }
        sender.sendMessage("Stopping the background profiler before starting... please wait");
        std::string cancel_error;
        if (!profiler_.cancel(cancel_error)) {
            sender.sendErrorMessage("Couldn't stop the background profiler safely: {}", cancel_error);
            return;
        }
        session_type_ = SessionType::None;
        background_started_ = false;
    }

    std::vector<NativePluginSource> native_plugin_sources = metadata_provider_.nativePluginSources();
    std::string error;
    if (!profiler_.start(options, tid, error)) {
        sender.sendErrorMessage("Couldn't start the profiler: {}", error);
        return;
    }
    session_native_plugin_sources_ = std::move(native_plugin_sources);
    start_sender_name_ = sender.getName();
    start_sender_is_player_ = sender.isPlayer();
    session_type_ = SessionType::Foreground;
    background_suppressed_ = background_enabled_;

    if (options.alloc) {
        if (options.alloc_live_only) {
            sender.sendMessage("{}Retained Allocation Profiler is now running!{} (async)", kColorGold, kColorGray);
        }
        else {
            sender.sendMessage("{}Allocation Profiler is now running!{} (async)", kColorGold, kColorGray);
        }
        if (options.threads.empty() || (options.threads.size() == 1 && options.threads.front() == "*")) {
            sender.sendMessage("Sampling approximately every {} of native allocations across process threads.",
                               spark::formatBytes(static_cast<std::uint64_t>(options.allocation_interval_bytes)));
        }
        else {
            sender.sendMessage("Sampling approximately every {} of native allocations from matching threads.",
                               spark::formatBytes(static_cast<std::uint64_t>(options.allocation_interval_bytes)));
        }
        if (options.alloc_live_only) {
            sender.sendMessage("The result will contain only sampled allocations still live when profiling stops.");
        }
    }
    else {
        if (options.threads.empty()) {
            sender.sendMessage("{}Profiler is now running!{} (async, {}ms interval)", kColorGold, kColorGray,
                               options.interval_ms);
        }
        else if (options.threads.size() == 1 && options.threads.front() == "*") {
            sender.sendMessage("{}Profiler is now running for all process threads!{} (async, {}ms interval)",
                               kColorGold, kColorGray, options.interval_ms);
        }
        else {
            sender.sendMessage("{}Profiler is now running for selected process threads!{} (async, {}ms interval)",
                               kColorGold, kColorGray, options.interval_ms);
        }
    }
    if (options.only_ticks_over_ms > 0) {
        sender.sendMessage("Only recording ticks longer than {}ms.", options.only_ticks_over_ms);
    }
    if (timeout <= 0) {
        sender.sendMessage("It runs in the background until stopped.");
        sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", kColorGray);
        sender.sendMessage("To view the profile while it runs, run: {}/spark profiler open", kColorGray);
    }
    else {
        if (timeout < 30) {
            sender.sendMessage("Tip: a timeout over 30s gives noticeably more accurate results.");
        }
        sender.sendMessage("Results will be returned automatically after {}.", spark::formatDuration(timeout));
    }
}

void ProfilerService::cmdStop(CommandSender &sender, const Arguments &args)
{
    if (!profiler_.running()) {
        sender.sendMessage(exporting_.load() ? "The profiler has stopped; results are still being finalized."
                                             : "There isn't an active profiler running.");
        return;
    }
    std::string backend_error;
    if (profiler_.backendFailure(backend_error)) {
        std::string cleanup_error;
        if (!profiler_.cancel(cleanup_error)) {
            sender.sendMessage("{}Allocation profiler status: FAILED", kColorRed);
            sender.sendMessage("Unable to discard the failed session safely: {}", cleanup_error);
            return;
        }
        sender.sendMessage("{}Allocation profiler status: FAILED", kColorRed);
        sender.sendMessage("Incomplete profile data was discarded: {}", backend_error);
        sender.sendMessage("The allocation profiler backend is ready for a new session.");
        return;
    }
    bool save = profiler_.options().save_to_file || args.boolFlag("save-to-file");
    std::string comment;
    auto comments = args.stringFlag("comment");
    if (!comments.empty()) {
        comment = comments.front();
    }
    sender.sendMessage("{}Stopping the profiler and finalizing results, please wait...", kColorGold);
    closeViewerSocket();
    if (background_enabled_) {
        restart_background_after_export_ = true;
    }
    finishProfiler(sender.getName(), sender.isPlayer(), save, comment);
}

void ProfilerService::cmdInfo(CommandSender &sender)
{
    if (!profiler_.running()) {
        if (exporting_.load()) {
            sender.sendMessage("The profiler has stopped; results are still being finalized.");
            return;
        }
        sender.sendMessage("The profiler isn't running!");
        sender.sendMessage("To start a new one, run: {}/spark profiler start", kColorGray);
        return;
    }
    const bool allocation = profiler_.mode() == spark::ProfileMode::Allocation;
    std::string backend_error;
    if (allocation && profiler_.backendFailure(backend_error)) {
        sender.sendMessage("{}Allocation Profiler status: FAILED", kColorRed);
        sender.sendMessage("Backend service failure: {}", backend_error);
        sendAllocationHookCoverage(sender);
        sender.sendMessage("The incomplete profile will not be exported.");
        sender.sendMessage("Run {}/spark profiler stop{} or {}/spark profiler cancel{} to discard it.", kColorGray,
                           kColorReset, kColorGray, kColorReset);
        return;
    }
    if (allocation) {
        if (profiler_.options().alloc_live_only) {
            sender.sendMessage("{}Retained Allocation Profiler is already running!", kColorGold);
        }
        else {
            sender.sendMessage("{}Allocation Profiler is already running!", kColorGold);
        }
        sendAllocationHookCoverage(sender);
        const auto &threads = profiler_.options().threads;
        if (threads.empty() || (threads.size() == 1 && threads.front() == "*")) {
            sender.sendMessage("Thread selection: all process threads.");
        }
        else {
            sender.sendMessage("Thread selection: {} {} selector{} (matched at aggregation).", threads.size(),
                               profiler_.options().regex ? "regex" : "exact-name", threads.size() == 1 ? "" : "s");
        }
    }
    else {
        sender.sendMessage("{}Profiler is already running!", kColorGold);
    }
    std::int64_t ran = (nowMs() - profiler_.startTimeMs()) / 1000;
    if (!allocation && session_type_ == SessionType::Background) {
        sender.sendMessage("It was started automatically when spark enabled and has been "
                           "running in the background for {}.",
                           spark::formatDuration(ran));
    }
    if (allocation) {
        if (profiler_.options().alloc_live_only) {
            sender.sendMessage(
                "So far it has profiled for {} ({} tracked sampled allocations still live process-wide, {} estimated).",
                spark::formatDuration(ran), profiler_.liveAllocationSamples(),
                spark::formatBytes(profiler_.liveAllocationBytes()));
        }
        else {
            sender.sendMessage("So far it has profiled for {} ({} selected allocation samples, {} estimated; {} "
                               "observed process-wide).",
                               spark::formatDuration(ran), profiler_.sampleCount(),
                               spark::formatBytes(profiler_.sampledAllocationBytes()),
                               spark::formatBytes(profiler_.observedAllocationBytes()));
        }
        sender.sendMessage("Process-wide tracked lifecycle: {} freed, {} still live ({}).",
                           profiler_.freedAllocationSamples(), profiler_.liveAllocationSamples(),
                           spark::formatBytes(profiler_.liveAllocationBytes()));
        if (profiler_.droppedSamples() != 0) {
            sender.sendMessage("Dropped allocation samples: {}", profiler_.droppedSamples());
        }
        if (profiler_.filteredAllocationSamples() != 0) {
            sender.sendMessage("Allocation samples excluded by thread selector: {}.",
                               profiler_.filteredAllocationSamples());
        }
        if (profiler_.allocationThreadNameFailures() != 0) {
            sender.sendMessage("Allocation-origin thread names unavailable (failed closed for named selectors): {}.",
                               profiler_.allocationThreadNameFailures());
        }
    }
    else {
        sender.sendMessage("So far it has profiled for {} ({} samples).", spark::formatDuration(ran),
                           profiler_.sampleCount());
    }
    std::int64_t auto_end = profiler_.autoEndTimeMs();
    if (auto_end <= 0) {
        sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", kColorGray);
    }
    else {
        sender.sendMessage("It finishes automatically in {}.", spark::formatDuration((auto_end - nowMs()) / 1000));
    }
    sender.sendMessage("To cancel without generating a profile, run: {}/spark profiler cancel", kColorGray);
}

void ProfilerService::sendAllocationHookCoverage(CommandSender &sender)
{
    const auto &capabilities = profiler_.allocationHookCapabilities();
    std::size_t active = 0;
    std::size_t aliases = 0;
    std::string unavailable;
    for (const spark::AllocationHookCapability &capability : capabilities) {
        if (capability.status == spark::AllocationHookStatus::Active) {
            ++active;
        }
        else if (capability.status == spark::AllocationHookStatus::Alias) {
            ++aliases;
        }
        else {
            if (!unavailable.empty()) {
                unavailable += ", ";
            }
            unavailable += capability.name;
            unavailable += '=';
            unavailable += spark::allocationHookStatusName(capability.status);
        }
    }
    sender.sendMessage("Native allocation hooks: {}/{} entry points covered ({} patched targets, {} aliases).",
                       active + aliases, capabilities.size(), profiler_.allocationHookTargetCount(), aliases);
    if (!unavailable.empty()) {
        sender.sendMessage("Unavailable optional hooks: {}", unavailable);
    }
}

void ProfilerService::cmdCancel(CommandSender &sender)
{
    if (!profiler_.running()) {
        sender.sendMessage("There isn't an active profiler running.");
        return;
    }
    std::string backend_error;
    const bool failed = profiler_.backendFailure(backend_error);
    std::string error;
    if (!profiler_.cancel(error)) {
        sender.sendMessage("{}Unable to cancel the profiler safely: {}", kColorRed, error);
        return;
    }
    session_type_ = SessionType::None;
    background_started_ = false;
    background_suppressed_ = background_enabled_;
    closeViewerSocket();
    if (failed) {
        sender.sendMessage("{}Failed allocation profile data was discarded: {}", kColorRed, backend_error);
        sender.sendMessage("The allocation profiler backend is ready for a new session.");
    }
    else {
        sender.sendMessage("{}Profiler has been cancelled.", kColorGold);
    }
}

void ProfilerService::cmdOpen(CommandSender &sender)
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

    ExportContext context = captureLiveContext(nowMs());
    if (!startViewerWorker()) {
        sender.sendErrorMessage("Failed to start the live viewer worker.");
        return;
    }
    if (!viewer_worker_->enqueueOpen(std::move(context), std::move(socket), sender.getName())) {
        sender.sendErrorMessage("Failed to start the live viewer worker.");
        return;
    }
    sender.sendMessage("{}Opening the live viewer...{}", kColorGold, kColorGray);
}

bool ProfilerService::startViewerWorker()
{
    return viewer_worker_ && viewer_worker_->start();
}

void ProfilerService::stopViewerWorker()
{
    if (viewer_worker_) {
        viewer_worker_->stop();
    }
}

std::string ProfilerService::executeViewerWork(const ViewerUpdateWorker::WorkItem &work)
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

void ProfilerService::completeViewerWork(ViewerUpdateWorker::Completion completion) noexcept
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

void ProfilerService::completeViewerOpen(ViewerUpdateWorker::Completion completion)
{
    if (!viewer_worker_ || !viewer_worker_->completeOpen(completion.generation)) {
        return;
    }
    if (completion.url.empty() || !completion.socket || !completion.socket->isOpen() || !profiler_.running()) {
        if (completion.socket) {
            completion.socket->close();
        }
        notifier_.notify(completion.sender_name, "Failed to open the live viewer. Check your network connection.");
        return;
    }
    viewer_socket_ = std::move(completion.socket);
    viewer_sender_name_ = completion.sender_name;
    last_viewer_upload_ms_ = nowMs();
    notifier_.notify(completion.sender_name, "Live viewer opened! Open it at: " + completion.url);
    notifier_.notify(completion.sender_name, "The viewer updates every 10 seconds while the profiler is running.");
}

ExportContext ProfilerService::captureLiveContext(std::int64_t now_ms)
{
    ExportContext context;
    context.bds_executable_sha256 = bds_executable_sha256_;
    metadata_provider_.gatherServerMetadata(context, now_ms);
    context.native_plugin_sources = session_native_plugin_sources_;
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

std::string ProfilerService::uploadSamplerData(const ExportContext &context)
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

std::string ProfilerService::buildLiveSamplerData(const ExportContext &context)
{
    return profiler_.liveExport(context);
}

bool ProfilerService::viewerGenerationCurrent(std::uint64_t generation) const
{
    return viewer_worker_ && viewer_worker_->current(generation);
}

void ProfilerService::closeViewerSocket()
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
}

void ProfilerService::cmdTrustViewer(CommandSender &sender, const Arguments &args)
{
    auto ids = args.stringFlag("id");
    if (ids.empty()) {
        sender.sendMessage("Usage: /spark profiler trust-viewer --id <client id>");
        sender.sendMessage("Use the client id shown when a viewer connects.");
        return;
    }
    if (!viewer_socket_ || !viewer_socket_->isOpen()) {
        sender.sendMessage("No live viewer is currently open.");
        return;
    }
    for (const auto &id : ids) {
        auto key = viewer_socket_->pendingKey(id);
        if (key.empty()) {
            sender.sendMessage("No pending client found with id '{}'.", id);
            continue;
        }
        std::string b64 = base64Encode(key.data(), key.size());
        // Avoid duplicates.
        if (trusted_viewers_.contains(b64)) {
            sender.sendMessage("Client '{}' is already trusted.", id);
            continue;
        }
        trusted_viewers_.add(b64);
        trusted_viewers_.save();
        viewer_socket_->sendClientTrusted(id);
        sender.sendMessage("Client '{}' is now trusted.", id);
    }
}

void ProfilerService::finishProfiler(const std::string &sender_name, bool sender_is_player, bool save,
                                     const std::string &comment)
{
    std::string stop_error;
    if (!profiler_.stopSampling(stop_error)) {
        std::string backend_error;
        if (!profiler_.running() && profiler_.backendFailure(backend_error)) {
            notifier_.notify(sender_name,
                             "Allocation profiler FAILED; incomplete profile data was discarded: " + backend_error);
            notifier_.notify(sender_name, "The allocation profiler backend is ready for a new session.");
        }
        else {
            notifier_.notify(sender_name, "Profiler stop failed: " + stop_error);
        }
        // Restore the background profiler when export cannot start.
        background_started_ = false;
        if (restart_background_after_export_) {
            restart_background_after_export_ = false;
            background_suppressed_ = false;
        }
        return;
    }
    session_type_ = SessionType::None;

    pending_ctx_ = ExportContext{};
    pending_ctx_.bds_executable_sha256 = bds_executable_sha256_;
    metadata_provider_.gatherServerMetadata(pending_ctx_, nowMs());
    pending_ctx_.native_plugin_sources = session_native_plugin_sources_;
    pending_ctx_.comment = comment;
    pending_ctx_.statistics = statistics_.snapshot();
    pending_ctx_.metrics = statistics_.metricsSnapshot();
    pending_ctx_.window_stats = statistics_.profileWindows(profiler_.startTimeMs(), profiler_.endTimeMs());
    pending_ctx_.system_stats = spark::gatherSystemStats(".");
    metadata_provider_.gatherWorldMetadata(pending_ctx_);
    if (ping_samples_provider_) {
        pending_ctx_.ping_samples = ping_samples_provider_();
    }
    if (network_snapshot_provider_) {
        pending_ctx_.net_snapshots = network_snapshot_provider_();
    }

    pending_save_ = save;
    pending_sender_ = sender_name;
    pending_sender_is_player_ = sender_is_player;

    // Join any completed export thread before starting a new one.
    if (export_thread_.joinable()) {
        export_thread_.join();
    }

    exporting_.store(true);
    try {
        export_thread_ = std::thread([this] { runExport(); });
    }
    catch (...) {
        exporting_.store(false);
        if (restart_background_after_export_) {
            restart_background_after_export_ = false;
            background_suppressed_ = false;
            background_started_ = startBackgroundSession();
        }
        notifier_.notify(sender_name, "Failed to start the profile export worker.");
    }
}

void ProfilerService::runExport() noexcept
{
    try {
        ProfileExporter::Result result = exporter_.exportProfile(profiler_, pending_ctx_, pending_save_);
        pending_outcome_ = result.outcome;
        pending_result_ = std::move(result.message);
    }
    catch (const std::exception &error) {
        pending_outcome_ = ExportOutcome::Failed;
        pending_result_ = std::string("Export failed: ") + error.what();
    }
    catch (...) {
        pending_outcome_ = ExportOutcome::Failed;
        pending_result_ = "Export failed with an unknown error.";
    }
    const std::weak_ptr<int> lifetime = lifetime_;
    try {
        dispatcher_.runOnMainThread([this, lifetime]() {
            if (lifetime.expired()) {
                return;
            }
            announceResult();
        });
    }
    catch (...) {
        export_completion_pending_.store(true, std::memory_order_release);
    }
}

void ProfilerService::announceResult()
{
    const char *headline = "Profiler stopped.";
    if (pending_outcome_ == ExportOutcome::Uploaded) {
        headline = "Profiler stopped & upload complete!";
    }
    else if (pending_outcome_ == ExportOutcome::Saved) {
        headline = "Profiler stopped & saved locally!";
    }
    notifier_.notify(pending_sender_, headline);
    notifier_.notify(pending_sender_, pending_result_);

    // A successful export means the profile is safely delivered; discard the
    // crash-recovery journal so the next startup does not treat it as a crash.
    // On failure the journal is retained so a subsequent crash can still recover.
    if (pending_outcome_ == ExportOutcome::Uploaded || pending_outcome_ == ExportOutcome::Saved) {
        profiler_.discardRecoveryJournal();
    }

    if (activity_log_provider_) {
        ActivityLog *log = activity_log_provider_();
        if (log) {
            const std::int64_t now_ms = nowMs();
            if (pending_outcome_ == ExportOutcome::Uploaded) {
                log->add(
                    Activity::url(pending_sender_, pending_sender_is_player_, now_ms, "Profiler", pending_result_));
            }
            else if (pending_outcome_ == ExportOutcome::Saved) {
                log->add(
                    Activity::file(pending_sender_, pending_sender_is_player_, now_ms, "Profiler", pending_result_));
            }
        }
    }

    exporting_.store(false);

    if (restart_background_after_export_) {
        restart_background_after_export_ = false;
        background_suppressed_ = false;
        if (startBackgroundSession()) {
            // NOLINTNEXTLINE(readability-simplify-boolean-expr)
            background_started_ = true;
        }
        else {
            background_started_ = false;
        }
    }
}

void ProfilerService::onTick(double mspt)
{
    if (export_completion_pending_.exchange(false, std::memory_order_acq_rel)) {
        announceResult();
    }
    if (viewer_worker_ && viewer_worker_->consumeFailure()) {
        notifier_.notify(viewer_sender_name_.empty() ? start_sender_name_ : viewer_sender_name_,
                         "Live viewer worker failed.");
        closeViewerSocket();
    }
    if (!background_started_ && !background_suppressed_ && background_enabled_ && main_tid_ != 0 &&
        !profiler_.running() && !exporting_.load()) {
        auto now = nowMs();
        if (now >= next_background_retry_ms_) {
            if (startBackgroundSession()) {
                background_started_ = true;
                background_retry_delay_s_ = 0;
            }
            else {
                // Exponential backoff: 5s -> 15s -> 30s -> 60s (cap).
                if (background_retry_delay_s_ == 0) {
                    background_retry_delay_s_ = 5;
                }
                else if (background_retry_delay_s_ < 60) {
                    background_retry_delay_s_ = std::min(60, background_retry_delay_s_ * 2);
                }
                next_background_retry_ms_ = now + background_retry_delay_s_ * 1000;
            }
        }
    }

    if (!profiler_.running()) {
        return;
    }

    // Live viewer socket lifecycle.
    if (viewer_socket_) {
        if (!viewer_socket_->tick()) {
            std::string diagnostic = viewer_socket_->takeDiagnostic();
            if (!diagnostic.empty()) {
                notifier_.notify(viewer_sender_name_.empty() ? start_sender_name_ : viewer_sender_name_, diagnostic);
            }
            closeViewerSocket();
        }
        else if (viewer_socket_->isOpen()) {
            auto now = nowMs();
            if (now - last_viewer_upload_ms_ >= 10000) {
                const bool available = viewer_worker_ && viewer_worker_->available();
                const std::uint64_t generation = viewer_worker_ ? viewer_worker_->generation() : 0;
                if (available) {
                    ExportContext context = captureLiveContext(now);
                    if (viewer_worker_->enqueueUpdate(std::move(context), viewer_socket_, generation)) {
                        last_viewer_upload_ms_ = now;
                    }
                }
            }
        }
    }

    std::string backend_error;
    const bool backend_failed = profiler_.backendFailure(backend_error);
    if (!backend_failed) {
        profiler_.onTick(mspt);
    }
    std::int64_t auto_end = profiler_.autoEndTimeMs();
    if (auto_end > 0 && nowMs() >= auto_end) {
        bool save = profiler_.options().save_to_file;
        closeViewerSocket();
        finishProfiler(start_sender_name_, start_sender_is_player_, save, std::string());
    }
}

void ProfilerService::startBackgroundProfiler()
{
    background_suppressed_ = false;
    if (!background_enabled_) {
        return;
    }
    if (profiler_.running() || exporting_.load()) {
        return;
    }
    if (startBackgroundSession()) {
        background_started_ = true;
    }
    // If main_tid_ is 0, the background profiler will start on the first tick.
}

bool ProfilerService::startBackgroundSession()
{
    if (!background_enabled_ || profiler_.running() || exporting_.load()) {
        return false;
    }

    if (main_tid_ == 0) {
        return false;
    }

    spark::ProfilerOptions options;
    options.is_background = true;
    options.interval_ms = background_interval_;
    options.timeout_seconds = -1;
    options.ignore_sleeping = false;

    if (background_thread_dumper_ == "all") {
        options.threads = {"*"};
    }

    if (background_thread_grouper_ == "by-name") {
        options.thread_grouper = spark::ThreadGrouperMode::ByName;
    }
    else if (background_thread_grouper_ == "as-one") {
        options.thread_grouper = spark::ThreadGrouperMode::AsOne;
    }
    else {
        options.thread_grouper = spark::ThreadGrouperMode::ByPool;
    }

    std::vector<NativePluginSource> native_plugin_sources = metadata_provider_.nativePluginSources();
    std::string error;
    if (!profiler_.start(options, main_tid_, error)) {
        return false;
    }
    session_native_plugin_sources_ = std::move(native_plugin_sources);

    session_type_ = SessionType::Background;
    return true;
}

}  // namespace spark
