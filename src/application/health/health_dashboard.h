#ifndef SPARK_APPLICATION_HEALTH_HEALTH_DASHBOARD_H
#define SPARK_APPLICATION_HEALTH_HEALTH_DASHBOARD_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "application/health/health_dashboard_connection.h"
#include "net/cancellation.h"
#include "proto/health_data.h"

namespace spark {

struct HealthDashboardTestAccess;

class HealthDashboard {
public:
    struct OpenResult {
        bool accepted = false;
        bool completed = false;
        bool ok = false;
        std::uint64_t generation = 0;
        std::string sender_name;
        std::string url;
        std::string payload_key;
        std::string error;
    };

    using ConnectionFactory = std::function<std::unique_ptr<HealthDashboardConnection>()>;
    using InitialUpload = std::function<UploadResult(const HealthData &, CancellationToken)>;
    using IsKeyTrustedCallback = HealthDashboardConnection::IsKeyTrustedCallback;
    using CompletionCallback = std::function<void(OpenResult)>;

    HealthDashboard(ConnectionFactory connection_factory, InitialUpload initial_upload,
                    IsKeyTrustedCallback is_key_trusted, CompletionCallback completion);
    ~HealthDashboard();

    HealthDashboard(const HealthDashboard &) = delete;
    HealthDashboard &operator=(const HealthDashboard &) = delete;

    // Queues one connection open. The returned result describes whether work was accepted;
    // the completion callback receives the eventual URL or error.
    OpenResult open(HealthData initial, const std::string &sender_name);

    bool updateDue(std::int64_t now_ms) const;
    bool enqueueUpdate(HealthData snapshot, std::int64_t now_ms);
    void tick();
    void requestStop() noexcept;
    bool shutdownWithin(std::chrono::milliseconds timeout);
    void shutdown();

    bool isOpen() const;
    bool openPending() const;
    std::uint64_t generation() const;
    std::vector<std::uint8_t> pendingKey(const std::string &client_id) const;
    void sendClientTrusted(const std::string &client_id);
    bool consumeFailure() { return failed_.exchange(false, std::memory_order_acq_rel); }

private:
    friend struct HealthDashboardTestAccess;

    enum class WorkType {
        Open,
        Update,
    };

    struct WorkItem {
        WorkType type = WorkType::Update;
        HealthData data;
        std::shared_ptr<HealthDashboardConnection> connection;
        CancellationToken cancellation;
        std::uint64_t generation = 0;
        std::string sender_name;
    };

    void run() noexcept;
    bool startWorker();
    void signalWorkerExit() noexcept;
    void markFailure(const std::shared_ptr<HealthDashboardConnection> &connection) noexcept;
    void completeOpen(OpenResult result, const std::shared_ptr<HealthDashboardConnection> &connection,
                      std::int64_t initial_time_ms);

    ConnectionFactory connection_factory_;
    InitialUpload initial_upload_;
    IsKeyTrustedCallback is_key_trusted_;
    CompletionCallback completion_;

    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool lifecycle_active_ = false;
    std::thread::id worker_id_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::mutex completion_mutex_;
    std::thread worker_;
    std::mutex worker_exit_mutex_;
    std::condition_variable worker_exit_cv_;
    bool worker_exited_ = true;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::optional<WorkItem> work_;
    bool work_active_ = false;
    bool open_pending_ = false;
    bool open_ = false;
    bool stopping_ = false;
    bool shutdown_complete_ = false;
    bool callbacks_enabled_ = true;
    std::uint64_t generation_ = 0;
    std::int64_t last_update_ms_ = 0;
    CancellationSource cancellation_source_;
    std::shared_ptr<HealthDashboardConnection> connection_;

    static constexpr auto kDefaultShutdownBudget = std::chrono::seconds(2);
    static constexpr auto kBackgroundCloseBudget = std::chrono::milliseconds(500);
};

}  // namespace spark

#endif  // SPARK_APPLICATION_HEALTH_HEALTH_DASHBOARD_H
