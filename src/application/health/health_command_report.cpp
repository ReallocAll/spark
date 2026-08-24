#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "application/health/health_command.h"
#include "application/health/health_report.h"
#include "core/util/base64.h"
#include "core/util/format.h"
#include "core/util/monotonic_time.h"
#include "net/gzip.h"
#include "spark_constants.h"

namespace spark {

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

    const std::int64_t now_ms = monotonicUnixMillis();
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
    accepted_dashboard_generation_ = dashboard_->generation() + 1;
    const HealthDashboard::OpenResult result = dashboard_->open(std::move(initial), dashboard_sender_);
    if (!result.accepted) {
        dashboard_sender_.clear();
        accepted_dashboard_generation_ = 0;
        sender.sendMessage("Health dashboard could not be opened: {}", result.error);
        return;
    }
    accepted_dashboard_generation_ = result.generation;
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
        if (!trusted_viewers_.addAndSave(b64)) {
            const std::string error = trusted_viewers_.lastError();
            if (error.empty()) {
                sender.sendErrorMessage("Unable to persist trust for client '{}'.", id);
            }
            else {
                sender.sendErrorMessage("Unable to persist trust for client '{}': {}", id, error);
            }
            continue;
        }
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

    const std::int64_t now_ms = monotonicUnixMillis();
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
            if (!dashboard_ || result.generation == 0 || result.generation != accepted_dashboard_generation_ ||
                result.generation != dashboard_->generation()) {
                return;
            }
            if (!result.ok) {
                dashboard_sender_.clear();
                accepted_dashboard_generation_ = 0;
                try {
                    notifier_.notify(result.sender_name, "Failed to open the health dashboard.");
                }
                catch (...) {  // NOLINT(bugprone-empty-catch): completion notification is best effort.
                }
                return;
            }
            try {
                notifier_.notify(result.sender_name, "Health dashboard opened! Open it at: " + result.url);
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): completion notification is best effort.
            }
            try {
                if (activity_log_provider_) {
                    ActivityLog *log = activity_log_provider_();
                    if (log) {
                        log->add(Activity::url(result.sender_name, dashboard_sender_is_player_, dashboard_open_time_ms_,
                                               "Health report", result.url));
                    }
                }
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): activity logging is best effort.
            }
        });
    }
    catch (...) {  // NOLINT(bugprone-empty-catch): dispatch failure leaves the result unannounced.
    }
}

void HealthCommand::announceHealthUpload() noexcept
{
    uploading_.store(false);
    UploadResult result;
    std::string sender_name;
    bool sender_is_player = false;
    std::int64_t now_ms = 0;
    try {
        {
            std::scoped_lock lock(upload_mutex_);
            result = std::move(upload_result_);
            sender_name = std::move(upload_sender_);
            sender_is_player = upload_sender_is_player_;
            now_ms = upload_time_ms_;
        }
        const auto notify_best_effort = [this, &sender_name](const std::string &message) noexcept {
            try {
                notifier_.notify(sender_name, message);
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): completion notification is best effort.
            }
        };
        if (result.ok) {
            const std::string url = viewer_url_ + result.key;
            notify_best_effort("Health report uploaded! " + url);
            try {
                if (activity_log_provider_) {
                    ActivityLog *log = activity_log_provider_();
                    if (log) {
                        log->add(Activity::url(sender_name, sender_is_player, now_ms, "Health report", url));
                    }
                }
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): activity logging is best effort.
            }
        }
        else {
            notify_best_effort("Health report upload failed: " + result.error);
        }
    }
    catch (...) {  // NOLINT(bugprone-empty-catch): completion notifications are best effort.
    }
}

}  // namespace spark
