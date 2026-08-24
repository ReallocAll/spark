#ifndef SPARK_TESTS_APPLICATION_HEALTH_HEALTH_DASHBOARD_TEST_SUPPORT_H
#define SPARK_TESTS_APPLICATION_HEALTH_HEALTH_DASHBOARD_TEST_SUPPORT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "application/health/health_dashboard.h"

namespace spark {

struct HealthDashboardTestAccess {
    static bool stopping(const HealthDashboard &dashboard);
};

namespace health_dashboard_test {

class FakeConnection;

struct Probe {
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<FakeConnection> latest;
    std::vector<spark::HealthDashboard::OpenResult> completions;
    std::optional<spark::SocketChannelInfo> uploaded_channel;
    bool next_open_success = true;
    std::atomic<bool> close_during_work{false};
};

class FakeConnection final : public spark::HealthDashboardConnection {
public:
    explicit FakeConnection(Probe &probe);

    std::string open(const UploadCallback &upload) override;
    bool tick() override;
    bool isOpen() const override;
    bool hasClient() const override;
    void close() override;
    spark::SocketChannelInfo channelInfo() const override;
    bool sendStatistics(const std::string &, const std::string &, const std::string &) override;
    std::vector<std::uint8_t> pendingKey(const std::string &client_id) const override;
    void sendClientTrusted(const std::string &client_id) override;
    void setIsKeyTrustedCallback(IsKeyTrustedCallback callback) override;

    void configureOpen(bool success, bool block);
    void configureSend(bool fail, bool block);
    void setClient(bool client);
    void releaseOpen();
    void releaseSend();
    bool waitOpenEntered();
    bool waitSendEntered();
    int sendCount() const;

private:
    Probe &probe_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    spark::HealthDashboardConnection::IsKeyTrustedCallback trusted_callback_;
    bool open_success_ = true;
    bool open_state_ = false;
    bool client_ = false;
    bool block_open_ = false;
    bool release_open_ = true;
    bool open_entered_ = false;
    bool fail_send_ = false;
    bool block_send_ = false;
    bool release_send_ = true;
    bool send_entered_ = false;
    bool work_active_ = false;
    int send_count_ = 0;
    int close_count_ = 0;
    std::string trusted_client_;
};

template <typename Predicate>
bool waitFor(Probe &probe, Predicate predicate)
{
    std::unique_lock lock(probe.mutex);
    return probe.cv.wait_for(lock, std::chrono::seconds(2), std::move(predicate));
}

spark::HealthData dataAt(std::int64_t generated_time_ms);
std::unique_ptr<spark::HealthDashboard> makeDashboard(Probe &probe);

}  // namespace health_dashboard_test
}  // namespace spark

#endif  // SPARK_TESTS_APPLICATION_HEALTH_HEALTH_DASHBOARD_TEST_SUPPORT_H
