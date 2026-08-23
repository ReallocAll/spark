#include "application/profiler/profiler_service.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/util/monotonic_time.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
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
    viewer_open_ = std::make_unique<ProfilerOpenOrchestrator>(
        profiler_, statistics_, bds_executable_sha256_, bytebin_url_, viewer_url_, bytesocks_host_, trusted_viewers_,
        dispatcher_, metadata_provider_, notifier_);
    viewer_open_->setNativePluginSourcesProvider([this]() { return session_native_plugin_sources_; });
    viewer_open_->setPingSamplesProvider(
        [this]() { return ping_samples_provider_ ? ping_samples_provider_() : std::vector<int>(); });
    viewer_open_->setNetworkSnapshotProvider([this]() {
        return network_snapshot_provider_ ? network_snapshot_provider_()
                                          : std::map<std::string, NetworkInterfaceSnapshot>();
    });
}

ProfilerService::~ProfilerService()
{
    shutdown();
}

void ProfilerService::shutdown()
{
    resetProfilerTimeout();
    profiler_.requestStop();
    lifetime_.reset();
    if (viewer_open_) {
        viewer_open_->shutdown();
    }
    if (export_thread_.joinable()) {
        export_thread_.join();
    }
    export_completion_pending_.store(false);
    exporting_.store(false);
    restart_background_after_export_ = false;
}

ExportContext ProfilerService::captureLiveContext(std::int64_t now_ms)
{
    return viewer_open_->captureLiveContext(now_ms);
}

std::string ProfilerService::buildLiveSamplerData(const ExportContext &context)
{
    return viewer_open_->buildLiveSamplerData(context);
}

void ProfilerService::closeViewerSocket()
{
    if (viewer_open_) {
        viewer_open_->close();
    }
}

void ProfilerService::resetProfilerTimeout() noexcept
{
    profiler_timeout_.cancel();
    timeout_completion_pending_.store(false, std::memory_order_release);
}

bool ProfilerService::armProfilerTimeout(std::int64_t timeout_seconds) noexcept
{
    if (timeout_seconds <= 0) {
        timeout_completion_pending_.store(false, std::memory_order_release);
        return true;
    }

    using MillisecondsRep = std::chrono::milliseconds::rep;
    constexpr std::int64_t k_milliseconds_per_second = 1000;
    constexpr auto k_maximum_seconds =
        static_cast<std::int64_t>(std::numeric_limits<MillisecondsRep>::max() / k_milliseconds_per_second);
    if (timeout_seconds > k_maximum_seconds) {
        return false;
    }

    timeout_completion_pending_.store(false, std::memory_order_release);
    const auto delay = std::chrono::milliseconds(static_cast<MillisecondsRep>(timeout_seconds) *
                                                 static_cast<MillisecondsRep>(k_milliseconds_per_second));
    return profiler_timeout_.arm(delay, [this]() noexcept {
        profiler_.requestStop();
        timeout_completion_pending_.store(true, std::memory_order_release);
    });
}

void ProfilerService::finishProfiler(const std::string &sender_name, bool sender_is_player, bool save,
                                     const std::string &comment)
{
    resetProfilerTimeout();
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
    if (timeout_completion_pending_.exchange(false, std::memory_order_acq_rel)) {
        resetProfilerTimeout();
        if (profiler_.running()) {
            const bool save = profiler_.options().save_to_file;
            closeViewerSocket();
            finishProfiler(start_sender_name_, start_sender_is_player_, save, std::string());
        }
    }
    if (viewer_open_) {
        viewer_open_->onTick(start_sender_name_);
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

    resetProfilerTimeout();

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
