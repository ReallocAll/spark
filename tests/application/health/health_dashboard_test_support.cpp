#include "health_dashboard_test_support.h"

#include <chrono>
#include <utility>

namespace spark {

bool HealthDashboardTestAccess::idle(const HealthDashboard &dashboard)
{
    std::scoped_lock lock(dashboard.mutex_);
    return !dashboard.work_.has_value() && !dashboard.work_active_;
}

bool HealthDashboardTestAccess::shutdownActive(HealthDashboard &dashboard)
{
    std::scoped_lock lock(dashboard.lifecycle_mutex_);
    return dashboard.lifecycle_active_;
}

bool HealthDashboardTestAccess::stopping(const HealthDashboard &dashboard)
{
    std::scoped_lock lock(dashboard.mutex_);
    return dashboard.stopping_;
}

namespace health_dashboard_test {

void Probe::configureFactory(bool block)
{
    std::scoped_lock lock(mutex);
    block_factory = block;
    release_factory = !block;
    factory_entered = false;
}

void Probe::releaseFactory()
{
    std::scoped_lock lock(mutex);
    release_factory = true;
    cv.notify_all();
}

bool Probe::waitFactoryEntered()
{
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [this] { return factory_entered; });
}

bool Probe::waitConnectionReady()
{
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [this] { return static_cast<bool>(latest); });
}

void Probe::configureUpload(bool block)
{
    std::scoped_lock lock(mutex);
    block_upload = block;
    release_upload = !block;
    upload_entered = false;
    upload_cancelled.store(false, std::memory_order_release);
}

void Probe::releaseUpload()
{
    std::scoped_lock lock(mutex);
    release_upload = true;
    cv.notify_all();
}

bool Probe::waitUploadEntered()
{
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [this] { return upload_entered; });
}

FakeConnection::FakeConnection(Probe &probe) : probe_(probe) {}

std::string FakeConnection::open(const UploadCallback &upload, const spark::CancellationToken &cancellation)
{
    {
        std::unique_lock lock(mutex_);
        work_active_ = true;
        open_entered_ = true;
        cv_.notify_all();
        probe_.cv.notify_all();
        cv_.wait(lock, [this, &cancellation] {
            return !block_open_ || release_open_ || stop_requested_ || cancellation.stopRequested();
        });
        if (stop_requested_ || cancellation.stopRequested()) {
            work_active_ = false;
            cv_.notify_all();
            probe_.cv.notify_all();
            return {};
        }
    }
    const spark::UploadResult result = upload(cancellation);
    std::scoped_lock lock(mutex_);
    work_active_ = false;
    cv_.notify_all();
    if (stop_requested_ || cancellation.stopRequested() || !open_success_ || !result.ok) {
        return {};
    }
    open_state_ = true;
    return "https://viewer/" + result.key;
}

bool FakeConnection::tick()
{
    std::scoped_lock lock(mutex_);
    return open_state_ && !stop_requested_;
}

bool FakeConnection::isOpen() const
{
    std::scoped_lock lock(mutex_);
    return open_state_ && !stop_requested_;
}

bool FakeConnection::hasClient() const
{
    std::scoped_lock lock(mutex_);
    return client_;
}

void FakeConnection::requestStop() noexcept
{
    std::scoped_lock lock(mutex_);
    stop_requested_ = true;
    cv_.notify_all();
}

bool FakeConnection::closeWithin(std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return !work_active_; })) {
        return false;
    }
    open_state_ = false;
    ++close_count_;
    cv_.notify_all();
    return true;
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
    probe_.cv.notify_all();
    cv_.wait(lock, [this] { return !block_send_ || release_send_ || stop_requested_; });
    if (stop_requested_) {
        work_active_ = false;
        cv_.notify_all();
        probe_.cv.notify_all();
        return false;
    }
    ++send_count_;
    work_active_ = false;
    cv_.notify_all();
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
    probe_.trusted_send_count.fetch_add(1, std::memory_order_acq_rel);
    cv_.notify_all();
    probe_.cv.notify_all();
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

std::string FakeConnection::trustedClient() const
{
    std::scoped_lock lock(mutex_);
    return trusted_client_;
}

spark::HealthData dataAt(std::int64_t generated_time_ms)
{
    spark::HealthData data;
    data.generated_time_ms = generated_time_ms;
    data.platform_stats.present = true;
    data.system_stats.present = true;
    return data;
}

std::unique_ptr<spark::HealthDashboard> makeDashboard(Probe &probe,
                                                      spark::HealthDashboard::CompletionCallback completion)
{
    auto factory = [&probe] {
        bool open_success = true;
        bool open_block = false;
        {
            std::unique_lock lock(probe.mutex);
            probe.factory_entered = true;
            probe.cv.notify_all();
            probe.cv.wait(lock, [&probe] { return !probe.block_factory || probe.release_factory; });
            open_success = probe.next_open_success;
            open_block = probe.next_open_block;
        }
        auto connection = std::make_unique<FakeConnection>(probe);
        connection->configureOpen(open_success, open_block);
        {
            std::scoped_lock lock(probe.mutex);
            probe.latest = std::shared_ptr<FakeConnection>(connection.get(), [](FakeConnection *) {});
            probe.cv.notify_all();
        }
        return std::unique_ptr<spark::HealthDashboardConnection>(std::move(connection));
    };
    auto upload = [&probe](const spark::HealthData &data, const spark::CancellationToken &cancellation) {
        bool block = false;
        {
            std::scoped_lock lock(probe.mutex);
            probe.uploaded_channel = data.channel_info;
            probe.upload_entered = true;
            block = probe.block_upload;
            probe.cv.notify_all();
        }
        if (block && cancellation.waitForStop(std::chrono::seconds(2))) {
            probe.upload_cancelled.store(true, std::memory_order_release);
            return spark::UploadResult{.error = "cancelled"};
        }
        if (cancellation.stopRequested()) {
            probe.upload_cancelled.store(true, std::memory_order_release);
            return spark::UploadResult{.error = "cancelled"};
        }
        return spark::UploadResult{.ok = true, .key = "initial-key"};
    };
    if (!completion) {
        completion = [&probe](spark::HealthDashboard::OpenResult result) {
            std::scoped_lock lock(probe.mutex);
            probe.completions.push_back(std::move(result));
            probe.cv.notify_all();
        };
    }
    return std::make_unique<spark::HealthDashboard>(
        std::move(factory), std::move(upload), [](const std::vector<std::uint8_t> &) { return true; },
        std::move(completion));
}

}  // namespace health_dashboard_test
}  // namespace spark
