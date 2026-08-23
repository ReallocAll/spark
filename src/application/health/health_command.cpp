#include "application/health/health_command.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "application/health/health_report.h"
#include "core/command/arguments.h"
#include "core/stats/ping_statistics.h"
#include "core/util/base64.h"
#include "core/util/format.h"
#include "core/ws/crypto.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "proto/sampler_data.h"
#include "spark_constants.h"

namespace spark {

HealthCommand::HealthCommand(StatisticsService &statistics, ProfileMetadataProvider &metadata_provider,
                             std::string bytebin_url, std::string viewer_url, std::string bytesocks_host,
                             TrustedViewersState &trusted_viewers, MainThreadDispatcher &dispatcher,
                             ResultNotifier &notifier, HealthDashboard::ConnectionFactory dashboard_factory,
                             UploadFunction upload_function)
    : statistics_(statistics), metadata_provider_(metadata_provider), trusted_viewers_(trusted_viewers),
      dispatcher_(dispatcher), notifier_(notifier), bytebin_url_(std::move(bytebin_url)),
      viewer_url_(std::move(viewer_url)), bytesocks_host_(std::move(bytesocks_host))
{
    upload_fn_ = upload_function ? std::move(upload_function) : UploadFunction(uploadToBytebin);
    if (!dashboard_factory) {
        const std::string bytesocks_host_copy = bytesocks_host_;
        const std::string bytebin_url_copy = bytebin_url_;
        const std::string viewer_url_copy = viewer_url_;
        dashboard_factory = [bytesocks_host_copy, bytebin_url_copy,
                             viewer_url_copy]() -> std::unique_ptr<HealthDashboardConnection> {
            Crypto::KeyPair key_pair = Crypto::generateKeyPair();
            if (key_pair.public_key_x509.empty() || key_pair.private_key_pkcs8.empty()) {
                return std::unique_ptr<HealthDashboardConnection>();
            }
            ViewerSocket::Config config;
            config.bytesocks_host = bytesocks_host_copy;
            config.bytebin_url = bytebin_url_copy;
            config.viewer_url = viewer_url_copy;
            config.user_agent = std::string("endstone-spark/") + kVersion;
            config.sampler_interval = 0;
            config.statistics_interval = 10;
            return makeHealthDashboardViewerSocketConnection(std::move(config), std::move(key_pair));
        };
    }
    dashboard_ = std::make_unique<HealthDashboard>(
        std::move(dashboard_factory), [this](const HealthData &data) { return uploadHealthData(data); },
        [this](const std::vector<std::uint8_t> &key) {
            return trusted_viewers_.contains(base64Encode(key.data(), key.size()));
        },
        [this](HealthDashboard::OpenResult result) { completeHealthDashboard(std::move(result)); });
    // Lazily create PingStatistics if the platform provides a PlayerPingProvider.
    if (auto *ping_provider = metadata_provider_.playerPingProvider()) {
        ping_statistics_ = std::make_unique<PingStatistics>(*ping_provider);
    }
}

HealthCommand::~HealthCommand()
{
    shutdown();
}

void HealthCommand::shutdown()
{
    if (dashboard_) {
        dashboard_->shutdown();
    }
    lifetime_.reset();
    if (upload_thread_.joinable() && upload_thread_.get_id() != std::this_thread::get_id()) {
        upload_thread_.join();
    }
}

void HealthCommand::onTick()
{
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    onTickAt(now);
}

void HealthCommand::onTickAt(std::int64_t now_ms)
{
    if (!dashboard_) {
        return;
    }
    dashboard_->tick();
    if (dashboard_->updateDue(now_ms)) {
        try {
            HealthData snapshot = captureHealthDataForSender(dashboard_sender_, dashboard_sender_is_player_, now_ms);
            dashboard_->enqueueUpdate(std::move(snapshot), now_ms);
        }
        catch (...) {
        }
    }
    if (dashboard_->consumeFailure()) {
        const std::string sender = dashboard_sender_;
        dashboard_sender_.clear();
        if (!sender.empty()) {
            notifier_.notify(sender, "Health dashboard connection failed.");
        }
    }
}

void HealthCommand::pollPing()
{
    if (ping_statistics_ && ping_statistics_->poll()) {
        const PingSummary &summary = ping_statistics_->lastPollSummary();
        statistics_.recordPlayerPing({.mean = summary.mean(),
                                      .max = static_cast<double>(summary.max()),
                                      .min = static_cast<double>(summary.min()),
                                      .median = static_cast<double>(summary.median()),
                                      .percentile95 = static_cast<double>(summary.percentile95th())});
    }
}

std::vector<int> HealthCommand::pingSamples() const
{
    if (!ping_statistics_) {
        return {};
    }
    return ping_statistics_->rollingAverage().rawSamples();
}

void HealthCommand::pollNetwork()
{
    network_monitor_.poll();
}

std::map<std::string, NetworkInterfaceSnapshot> HealthCommand::networkSnapshots() const
{
    return network_monitor_.snapshot();
}

void HealthCommand::cmdTps(CommandSender &sender)
{
    sendPerformanceReport(sender, statistics_.snapshot());
}

void HealthCommand::cmdPing(CommandSender &sender, const Arguments &args)
{
    if (!ping_statistics_) {
        sender.sendMessage("{}Ping data is not available on this platform.{}", kColorGold, kColorGray);
        return;
    }

    // Query specific player
    auto players = args.stringFlag("player");
    if (!players.empty()) {
        for (const std::string &player_name : players) {
            PlayerPing ping = ping_statistics_->query(player_name);
            if (!ping.found()) {
                sender.sendMessage("{}Ping data is not available for '{}'.{}", kColorGold, kColorGray, kColorReset);
                sender.sendMessage("  {}", player_name);
            }
            else {
                sender.sendMessage("{}Player {}{} {}has {}{} ms ping.{}", kColorGold, kColorReset, ping.name,
                                   kColorGray, kColorGreen, ping.ping, kColorReset);
            }
        }
        return;
    }

    PingSummary summary = ping_statistics_->currentSummary();
    const PingRollingAverage &average = ping_statistics_->rollingAverage();

    if (summary.total() == 0 && average.samples() == 0) {
        sender.sendMessage("{}There is not enough data to show ping averages yet. Please try again later.{}",
                           kColorGold, kColorGray);
        return;
    }

    sender.sendMessage("{}Average Pings {}(min/med/95%ile/max ms){} from now, last 15m:", kColorGold, kColorGray,
                       kColorReset);
    sender.sendMessage("  {} ;  {}", formatPingRtts(summary), formatPingRtts(average));
}

void HealthCommand::showHealth(CommandSender &sender)
{
    showHealthReport(sender, statistics_, metadata_provider_, network_monitor_.snapshot());
}

void HealthCommand::cmdHealth(CommandSender &sender, const Arguments &args)
{
    std::string action = args.subCommand();
    if (action == "health") {
        action.clear();
    }
    if (action == "show") {
        showHealth(sender);
    }
    else if (action == "trust-viewer") {
        trustViewer(sender, args);
    }
    else if (action == "upload" || args.boolFlag("upload")) {
        uploadHealthReport(sender);
    }
    else {
        openHealthDashboard(sender);
    }
}

void HealthCommand::openHealthDashboard(CommandSender &sender)
{
    if (!dashboard_) {
        sender.sendErrorMessage("Health dashboard is unavailable.");
        return;
    }
    if (dashboard_->isOpen() || dashboard_->openPending()) {
        sender.sendMessage("A health dashboard is already open or opening.");
        return;
    }

    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    HealthData initial;
    try {
        initial = captureHealthData(sender, now_ms);
    }
    catch (const std::exception &error) {
        sender.sendErrorMessage("Health dashboard data generation failed: {}", error.what());
        return;
    }
    catch (...) {
        sender.sendErrorMessage("Health dashboard data generation failed.");
        return;
    }

    dashboard_sender_ = sender.getName();
    dashboard_sender_is_player_ = sender.isPlayer();
    dashboard_open_time_ms_ = now_ms;
    const HealthDashboard::OpenResult result = dashboard_->open(std::move(initial), dashboard_sender_);
    if (!result.accepted) {
        dashboard_sender_.clear();
        sender.sendMessage("Health dashboard could not be opened: {}", result.error);
        return;
    }
    sender.sendMessage("{}Opening the health dashboard...{}", kColorGold, kColorGray);
}

void HealthCommand::trustViewer(CommandSender &sender, const Arguments &args)
{
    const auto ids = args.stringFlag("id");
    if (ids.empty()) {
        sender.sendMessage("Usage: /spark health trust-viewer --id <client id>");
        sender.sendMessage("Use the client id shown when a health dashboard connects.");
        return;
    }
    if (!dashboard_ || !dashboard_->isOpen()) {
        sender.sendMessage("No health dashboard is currently open.");
        return;
    }
    for (const auto &id : ids) {
        const std::vector<std::uint8_t> key = dashboard_->pendingKey(id);
        if (key.empty()) {
            sender.sendMessage("No pending client found with id '{}'.", id);
            continue;
        }
        const std::string b64 = base64Encode(key.data(), key.size());
        if (trusted_viewers_.contains(b64)) {
            sender.sendMessage("Client '{}' is already trusted.", id);
            continue;
        }
        trusted_viewers_.add(b64);
        trusted_viewers_.save();
        dashboard_->sendClientTrusted(id);
        sender.sendMessage("Client '{}' is now trusted.", id);
    }
}

void HealthCommand::uploadHealthReport(CommandSender &sender)
{
    if (uploading_.exchange(true)) {
        sender.sendMessage("A health report upload is already in progress.");
        return;
    }
    if (upload_thread_.joinable()) {
        upload_thread_.join();
    }

    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    try {
        HealthData data = captureHealthData(sender, now_ms);
        const std::string sender_name = sender.getName();
        const bool sender_is_player = sender.isPlayer();
        upload_thread_ = std::thread([this, data = std::move(data), sender_name, sender_is_player, now_ms]() mutable {
            try {
                runHealthUpload(data, sender_name, sender_is_player, now_ms);
            }
            catch (...) {
                uploading_.store(false);
            }
        });
    }
    catch (const std::exception &error) {
        uploading_.store(false);
        sender.sendErrorMessage("Health report generation failed: {}", error.what());
        return;
    }
    sender.sendMessage("{}Health report upload started.{}", kColorGold, kColorGray);
}

HealthData HealthCommand::captureHealthData(const CommandSender &sender, std::int64_t now_ms)
{
    return captureHealthDataForSender(sender.getName(), sender.isPlayer(), now_ms);
}

HealthData HealthCommand::captureHealthDataForSender(const std::string &sender_name, bool sender_is_player,
                                                     std::int64_t now_ms)
{
    return spark::captureHealthData(statistics_, metadata_provider_, sender_name, sender_is_player, now_ms,
                                    pingSamples(), networkSnapshots());
}

UploadResult HealthCommand::uploadHealthData(const HealthData &data)
{
    UploadResult result;
    try {
        const std::string body = buildHealthData(data);
        const std::string compressed = gzipCompress(body);
        result = upload_fn_(compressed, bytebin_url_, kHealthContentType, std::string("endstone-spark/") + kVersion);
    }
    catch (const std::exception &error) {
        result.error = std::string("health report generation failed: ") + error.what();
    }
    catch (...) {
        result.error = "health report generation failed";
    }
    return result;
}

void HealthCommand::runHealthUpload(const HealthData &data, std::string sender_name, bool sender_is_player,
                                    std::int64_t now_ms)
{
    UploadResult result = uploadHealthData(data);
    {
        std::scoped_lock lock(upload_mutex_);
        upload_result_ = std::move(result);
        upload_sender_ = std::move(sender_name);
        upload_sender_is_player_ = sender_is_player;
        upload_time_ms_ = now_ms;
    }
    const std::weak_ptr<int> lifetime = lifetime_;
    try {
        dispatcher_.runOnMainThread([this, lifetime]() {
            if (lifetime.expired()) {
                return;
            }
            announceHealthUpload();
        });
    }
    catch (...) {
        uploading_.store(false);
    }
}

void HealthCommand::completeHealthDashboard(HealthDashboard::OpenResult result)
{
    const std::weak_ptr<int> lifetime = lifetime_;
    if (lifetime.expired()) {
        return;
    }
    try {
        dispatcher_.runOnMainThread([this, lifetime, result = std::move(result)]() mutable {
            if (lifetime.expired()) {
                return;
            }
            if (!result.ok) {
                notifier_.notify(result.sender_name, "Failed to open the health dashboard.");
                dashboard_sender_.clear();
                return;
            }
            notifier_.notify(result.sender_name, "Health dashboard opened! Open it at: " + result.url);
            if (activity_log_provider_) {
                ActivityLog *log = activity_log_provider_();
                if (log) {
                    log->add(Activity::url(result.sender_name, dashboard_sender_is_player_, dashboard_open_time_ms_,
                                           "Health report", result.url));
                }
            }
        });
    }
    catch (...) {
    }
}

void HealthCommand::announceHealthUpload()
{
    UploadResult result;
    std::string sender_name;
    bool sender_is_player = false;
    std::int64_t now_ms = 0;
    {
        std::scoped_lock lock(upload_mutex_);
        result = std::move(upload_result_);
        sender_name = std::move(upload_sender_);
        sender_is_player = upload_sender_is_player_;
        now_ms = upload_time_ms_;
    }
    if (result.ok) {
        const std::string url = viewer_url_ + result.key;
        notifier_.notify(sender_name, "Health report uploaded! " + url);
        if (activity_log_provider_) {
            ActivityLog *log = activity_log_provider_();
            if (log) {
                log->add(Activity::url(sender_name, sender_is_player, now_ms, "Health report", url));
            }
        }
    }
    else {
        notifier_.notify(sender_name, "Health report upload failed: " + result.error);
    }
    uploading_.store(false);
}

}  // namespace spark
