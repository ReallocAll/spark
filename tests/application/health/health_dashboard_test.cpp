#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "application/health/health_dashboard.h"

namespace {

using namespace std::chrono_literals;

struct Probe {
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<class FakeConnection> latest;
    std::vector<spark::HealthDashboard::OpenResult> completions;
    std::optional<spark::SocketChannelInfo> uploaded_channel;
    bool next_open_success = true;
};

class FakeConnection final : public spark::HealthDashboardConnection {
public:
    explicit FakeConnection(Probe &probe) : probe_(probe) {}

    std::string open(const UploadCallback &upload) override
    {
        {
            std::unique_lock lock(mutex_);
            open_entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return !block_open_ || release_open_; });
        }
        const spark::UploadResult result = upload();
        std::scoped_lock lock(mutex_);
        if (!open_success_) {
            return {};
        }
        open_state_ = true;
        return "https://viewer/" + result.key;
    }

    bool tick() override
    {
        std::scoped_lock lock(mutex_);
        return open_state_;
    }

    bool isOpen() const override
    {
        std::scoped_lock lock(mutex_);
        return open_state_;
    }

    bool hasClient() const override
    {
        std::scoped_lock lock(mutex_);
        return client_;
    }

    void close() override
    {
        std::scoped_lock lock(mutex_);
        open_state_ = false;
        ++close_count_;
        cv_.notify_all();
    }

    spark::SocketChannelInfo channelInfo() const override
    {
        return {.channel_id = "fake-channel", .public_key = {1, 2, 3}};
    }

    bool sendStatistics(const std::string &, const std::string &, const std::string &) override
    {
        std::unique_lock lock(mutex_);
        send_entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !block_send_ || release_send_; });
        ++send_count_;
        probe_.cv.notify_all();
        return !fail_send_ && open_state_ && client_;
    }

    std::vector<std::uint8_t> pendingKey(const std::string &client_id) const override
    {
        return client_id == "pending" ? std::vector<std::uint8_t>{9, 8, 7} : std::vector<std::uint8_t>{};
    }

    void sendClientTrusted(const std::string &client_id) override
    {
        std::scoped_lock lock(mutex_);
        trusted_client_ = client_id;
        cv_.notify_all();
    }

    void setIsKeyTrustedCallback(IsKeyTrustedCallback callback) override { trusted_callback_ = std::move(callback); }

    void configureOpen(bool success, bool block)
    {
        std::scoped_lock lock(mutex_);
        open_success_ = success;
        block_open_ = block;
        release_open_ = !block;
        open_entered_ = false;
    }

    void configureSend(bool fail, bool block)
    {
        std::scoped_lock lock(mutex_);
        fail_send_ = fail;
        block_send_ = block;
        release_send_ = !block;
        send_entered_ = false;
    }

    void setClient(bool client)
    {
        std::scoped_lock lock(mutex_);
        client_ = client;
    }

    void releaseOpen()
    {
        std::scoped_lock lock(mutex_);
        release_open_ = true;
        cv_.notify_all();
    }

    void releaseSend()
    {
        std::scoped_lock lock(mutex_);
        release_send_ = true;
        cv_.notify_all();
    }

    bool waitOpenEntered()
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 2s, [this] { return open_entered_; });
    }

    bool waitSendEntered()
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 2s, [this] { return send_entered_; });
    }

    int sendCount() const
    {
        std::scoped_lock lock(mutex_);
        return send_count_;
    }

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
    int send_count_ = 0;
    int close_count_ = 0;
    std::string trusted_client_;
};

template <typename Predicate>
bool waitFor(Probe &probe, Predicate predicate)
{
    std::unique_lock lock(probe.mutex);
    return probe.cv.wait_for(lock, 2s, std::move(predicate));
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

}  // namespace

int main()
{
    Probe probe;
    auto dashboard = makeDashboard(probe);

    const auto queued = dashboard->open(dataAt(0), "Alice");
    assert(queued.accepted);
    assert(dashboard->openPending());
    assert(probe.latest->waitOpenEntered());
    assert(!dashboard->open(dataAt(0), "Second").accepted);
    probe.latest->releaseOpen();
    assert(waitFor(probe, [&] { return probe.completions.size() == 1; }));
    assert(probe.completions[0].ok);
    assert(probe.completions[0].completed);
    assert(probe.completions[0].url == "https://viewer/initial-key");
    assert(probe.completions[0].payload_key == "initial-key");
    const auto uploaded_channel = probe.uploaded_channel;
    assert(uploaded_channel && uploaded_channel->channel_id == "fake-channel");
    if (!uploaded_channel) {
        return 1;
    }
    assert(uploaded_channel->public_key == std::vector<std::uint8_t>({1, 2, 3}));
    assert(dashboard->isOpen());

    probe.latest->setClient(false);
    assert(!dashboard->updateDue(10000));
    probe.latest->setClient(true);
    assert(!dashboard->updateDue(9999));
    assert(dashboard->updateDue(10000));
    assert(dashboard->enqueueUpdate(dataAt(10000), 10000));
    assert(!dashboard->enqueueUpdate(dataAt(10000), 20000));
    assert(waitFor(probe, [&] { return probe.latest->sendCount() == 1; }));

    probe.latest->configureSend(false, true);
    assert(dashboard->enqueueUpdate(dataAt(20000), 20000));
    assert(probe.latest->waitSendEntered());
    assert(!dashboard->enqueueUpdate(dataAt(30000), 30000));
    probe.latest->releaseSend();
    assert(waitFor(probe, [&] { return probe.latest->sendCount() == 2; }));

    probe.latest->configureSend(true, false);
    assert(dashboard->enqueueUpdate(dataAt(30000), 30000));
    assert(waitFor(probe, [&] { return dashboard->consumeFailure(); }));
    assert(!dashboard->isOpen());

    dashboard->shutdown();
    probe.next_open_success = true;
    const auto restarted = dashboard->open(dataAt(0), "Restart");
    assert(restarted.accepted);
    assert(probe.latest->waitOpenEntered());
    probe.latest->releaseOpen();
    assert(waitFor(probe, [&] { return probe.completions.size() == 2; }));
    assert(probe.completions.back().ok);
    probe.latest->setClient(true);

    probe.latest->configureSend(false, true);
    assert(dashboard->enqueueUpdate(dataAt(10000), 10000));
    assert(probe.latest->waitSendEntered());
    const std::size_t completions_before_shutdown = probe.completions.size();
    std::thread shutdown_thread([&] { dashboard->shutdown(); });
    probe.latest->releaseSend();
    shutdown_thread.join();
    assert(probe.completions.size() == completions_before_shutdown);

    probe.next_open_success = false;
    const auto failed = dashboard->open(dataAt(0), "Failure");
    assert(failed.accepted);
    assert(probe.latest->waitOpenEntered());
    probe.latest->releaseOpen();
    assert(waitFor(probe, [&] { return probe.completions.size() == completions_before_shutdown + 1; }));
    assert(!probe.completions.back().ok);
    dashboard->shutdown();

    probe.next_open_success = true;
    const auto trusted = dashboard->open(dataAt(0), "Trust");
    assert(trusted.accepted);
    assert(probe.latest->waitOpenEntered());
    probe.latest->releaseOpen();
    assert(waitFor(probe, [&] { return probe.completions.size() == completions_before_shutdown + 2; }));
    assert(dashboard->pendingKey("pending") == std::vector<std::uint8_t>({9, 8, 7}));
    dashboard->sendClientTrusted("pending");
    dashboard->shutdown();
    return 0;
}
