#include "health_dashboard_test_support.h"

#include <chrono>
#include <utility>

namespace spark {

bool HealthDashboardTestAccess::stopping(const HealthDashboard &dashboard)
{
    std::scoped_lock lock(dashboard.mutex_);
    return dashboard.stopping_;
}

namespace health_dashboard_test {

FakeConnection::FakeConnection(Probe &probe) : probe_(probe) {}

std::string FakeConnection::open(const UploadCallback &upload)
{
    {
        std::unique_lock lock(mutex_);
        work_active_ = true;
        open_entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !block_open_ || release_open_; });
    }
    const spark::UploadResult result = upload();
    std::scoped_lock lock(mutex_);
    work_active_ = false;
    if (!open_success_) {
        return {};
    }
    open_state_ = true;
    return "https://viewer/" + result.key;
}

bool FakeConnection::tick()
{
    std::scoped_lock lock(mutex_);
    return open_state_;
}

bool FakeConnection::isOpen() const
{
    std::scoped_lock lock(mutex_);
    return open_state_;
}

bool FakeConnection::hasClient() const
{
    std::scoped_lock lock(mutex_);
    return client_;
}

void FakeConnection::close()
{
    std::scoped_lock lock(mutex_);
    if (work_active_) {
        probe_.close_during_work.store(true, std::memory_order_release);
    }
    open_state_ = false;
    ++close_count_;
    cv_.notify_all();
}

spark::SocketChannelInfo FakeConnection::channelInfo() const
{
    return {.channel_id = "fake-channel", .public_key = {1, 2, 3}};
}

bool FakeConnection::sendStatistics(const std::string &, const std::string &, const std::string &)
{
    std::unique_lock lock(mutex_);
    work_active_ = true;
    send_entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return !block_send_ || release_send_; });
    ++send_count_;
    work_active_ = false;
    probe_.cv.notify_all();
    return !fail_send_ && open_state_ && client_;
}

std::vector<std::uint8_t> FakeConnection::pendingKey(const std::string &client_id) const
{
    return client_id == "pending" ? std::vector<std::uint8_t>{9, 8, 7} : std::vector<std::uint8_t>{};
}

void FakeConnection::sendClientTrusted(const std::string &client_id)
{
    std::scoped_lock lock(mutex_);
    trusted_client_ = client_id;
    cv_.notify_all();
}

void FakeConnection::setIsKeyTrustedCallback(IsKeyTrustedCallback callback)
{
    trusted_callback_ = std::move(callback);
}

void FakeConnection::configureOpen(bool success, bool block)
{
    std::scoped_lock lock(mutex_);
    open_success_ = success;
    block_open_ = block;
    release_open_ = !block;
    open_entered_ = false;
}

void FakeConnection::configureSend(bool fail, bool block)
{
    std::scoped_lock lock(mutex_);
    fail_send_ = fail;
    block_send_ = block;
    release_send_ = !block;
    send_entered_ = false;
}

void FakeConnection::setClient(bool client)
{
    std::scoped_lock lock(mutex_);
    client_ = client;
}

void FakeConnection::releaseOpen()
{
    std::scoped_lock lock(mutex_);
    release_open_ = true;
    cv_.notify_all();
}

void FakeConnection::releaseSend()
{
    std::scoped_lock lock(mutex_);
    release_send_ = true;
    cv_.notify_all();
}

bool FakeConnection::waitOpenEntered()
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(2), [this] { return open_entered_; });
}

bool FakeConnection::waitSendEntered()
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(2), [this] { return send_entered_; });
}

int FakeConnection::sendCount() const
{
    std::scoped_lock lock(mutex_);
    return send_count_;
}

spark::HealthData dataAt(std::int64_t generated_time_ms)
{
    spark::HealthData data;
    data.generated_time_ms = generated_time_ms;
    data.platform_stats.present = true;
    data.system_stats.present = true;
    return data;
}

std::unique_ptr<spark::HealthDashboard> makeDashboard(Probe &probe)
{
    auto factory = [&probe] {
        auto connection = std::make_unique<FakeConnection>(probe);
        {
            std::scoped_lock lock(probe.mutex);
            connection->configureOpen(probe.next_open_success, false);
            probe.latest = std::shared_ptr<FakeConnection>(connection.get(), [](FakeConnection *) {});
        }
        return std::unique_ptr<spark::HealthDashboardConnection>(std::move(connection));
    };
    auto upload = [&probe](const spark::HealthData &data) {
        {
            std::scoped_lock lock(probe.mutex);
            probe.uploaded_channel = data.channel_info;
            probe.cv.notify_all();
        }
        return spark::UploadResult{.ok = true, .key = "initial-key"};
    };
    auto completion = [&probe](spark::HealthDashboard::OpenResult result) {
        std::scoped_lock lock(probe.mutex);
        probe.completions.push_back(std::move(result));
        probe.cv.notify_all();
    };
    return std::make_unique<spark::HealthDashboard>(
        std::move(factory), std::move(upload), [](const std::vector<std::uint8_t> &) { return true; },
        std::move(completion));
}

}  // namespace health_dashboard_test
}  // namespace spark
