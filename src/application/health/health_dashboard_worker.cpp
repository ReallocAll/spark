#include <exception>

#include "application/health/health_dashboard.h"
#include "proto/metrics_proto.h"
#include "proto/statistics_proto.h"

namespace spark {

namespace {

std::string exceptionMessage(const std::exception &error)
{
    return error.what();
}

}  // namespace

void HealthDashboard::shutdown()
{
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    while (lifecycle_active_) {
        if (worker_id_ == std::this_thread::get_id()) {
            return;
        }
        lifecycle_cv_.wait(lifecycle_lock, [this] { return !lifecycle_active_; });
    }
    lifecycle_active_ = true;

    std::shared_ptr<HealthDashboardConnection> connection;
    {
        std::scoped_lock completion_lock(completion_mutex_);
        callbacks_enabled_ = false;
    }
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        ++generation_;
        work_.reset();
        open_pending_ = false;
        open_ = false;
        connection = std::move(connection_);
    }
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    const bool same_worker = worker_id_ == std::this_thread::get_id();
    const bool join_worker = !same_worker && worker_.joinable();
    lifecycle_lock.unlock();
    if (join_worker) {
        worker_.join();
    }
    if (connection) {
        try {
            connection->close();
        }
        catch (...) {  // NOLINT(bugprone-empty-catch): connection close is best effort during shutdown.
        }
    }
    {
        std::scoped_lock lock(mutex_);
        work_active_ = false;
        stopping_ = false;
    }
    lifecycle_lock.lock();
    lifecycle_active_ = false;
    if (join_worker) {
        worker_id_ = {};
    }
    lifecycle_lock.unlock();
    lifecycle_cv_.notify_all();
}

bool HealthDashboard::startWorker(std::unique_lock<std::mutex> &lifecycle_lock)
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (worker_.joinable() && worker_id_ == std::this_thread::get_id()) {
        return false;
    }
    if (worker_.joinable()) {
        lifecycle_active_ = true;
        lifecycle_lock.unlock();
        try {
            worker_.join();
        }
        catch (...) {
            lifecycle_lock.lock();
            lifecycle_active_ = false;
            lifecycle_cv_.notify_all();
            return false;
        }
        lifecycle_lock.lock();
        worker_id_ = {};
        lifecycle_active_ = false;
        lifecycle_cv_.notify_all();
    }
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            return false;
        }
        running_.store(true, std::memory_order_release);
    }
    try {
        worker_ = std::thread([this] { run(); });
    }
    catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    worker_id_ = worker_.get_id();
    return true;
}

void HealthDashboard::markFailure(const std::shared_ptr<HealthDashboardConnection> &connection) noexcept
{
    failed_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    bool current = false;
    {
        std::scoped_lock lock(mutex_);
        current = connection_ == connection;
        if (current) {
            open_ = false;
            open_pending_ = false;
            work_.reset();
            work_active_ = false;
        }
    }
    if (connection) {
        try {
            connection->close();
        }
        catch (...) {  // NOLINT(bugprone-empty-catch): connection close is best effort after failure.
        }
    }
    cv_.notify_all();
}

void HealthDashboard::completeOpen(OpenResult result, const std::shared_ptr<HealthDashboardConnection> &connection,
                                   std::int64_t initial_time_ms)
{
    bool current = false;
    {
        std::scoped_lock lock(mutex_);
        current = !stopping_ && result.generation == generation_ && connection_ == connection;
        if (current) {
            open_pending_ = false;
            work_active_ = false;
            open_ = result.ok;
            if (result.ok) {
                last_update_ms_ = initial_time_ms;
            }
            else {
                connection_.reset();
            }
        }
    }
    if (!current || !result.ok) {
        try {
            connection->close();
        }
        catch (...) {  // NOLINT(bugprone-empty-catch): connection close is best effort after an unsuccessful open.
        }
    }
    result.completed = true;
    CompletionCallback completion;
    {
        std::scoped_lock completion_lock(completion_mutex_);
        if (!callbacks_enabled_) {
            return;
        }
        completion = completion_;
    }
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || result.generation != generation_) {
            return;
        }
    }
    if (completion) {
        try {
            completion(std::move(result));
        }
        catch (...) {
            markFailure(connection);
        }
    }
}

void HealthDashboard::run() noexcept
{
    try {
        while (running_.load(std::memory_order_acquire)) {
            WorkItem work;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return !running_.load(std::memory_order_acquire) || work_.has_value(); });
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                if (!work_) {
                    continue;
                }
                work = std::move(*work_);
                work_.reset();
                work_active_ = true;
            }

            if (work.type == WorkType::Open) {
                OpenResult result;
                result.accepted = true;
                result.generation = work.generation;
                result.sender_name = work.sender_name;
                UploadResult upload_result;
                bool upload_called = false;
                try {
                    std::string url;
                    url = work.connection->open([&] {
                        HealthData upload_data = work.data;
                        upload_data.channel_info = work.connection->channelInfo();
                        upload_called = true;
                        upload_result = initial_upload_(upload_data);
                        return upload_result;
                    });
                    if (!upload_called) {
                        result.error = "health dashboard connection did not request initial upload";
                    }
                    else if (!upload_result.ok || upload_result.key.empty()) {
                        result.error =
                            upload_result.error.empty() ? "initial health upload failed" : upload_result.error;
                    }
                    else if (url.empty()) {
                        result.error = "health dashboard connection failed to open";
                    }
                    else {
                        result.ok = true;
                        result.url = url;
                        result.payload_key = upload_result.key;
                    }
                }
                catch (const std::exception &error) {
                    result.error = std::string("health dashboard open failed: ") + exceptionMessage(error);
                }
                catch (...) {
                    result.error = "health dashboard open failed";
                }
                completeOpen(std::move(result), work.connection, work.data.generated_time_ms);
                continue;
            }

            std::string platform;
            std::string system;
            std::string metrics;
            try {
                platform = proto_detail::buildPlatformStatistics(work.data.platform_stats, work.data.statistics);
                system = proto_detail::buildSystemStatistics(work.data.system_stats, work.data.statistics);
                metrics = proto_detail::buildMetrics(work.data.metrics);
            }
            catch (...) {
                markFailure(work.connection);
                continue;
            }

            bool sent = false;
            bool attempted = false;
            {
                std::scoped_lock lock(mutex_);
                attempted = !stopping_ && work.generation == generation_ && connection_ == work.connection && open_;
            }
            if (attempted) {
                sent = work.connection->sendStatistics(platform, system, metrics);
            }
            bool current = false;
            {
                std::scoped_lock lock(mutex_);
                current = !stopping_ && work.generation == generation_ && connection_ == work.connection;
                if (current) {
                    work_active_ = false;
                }
            }
            if (attempted && current && !sent) {
                markFailure(work.connection);
            }
        }
    }
    catch (...) {
        std::shared_ptr<HealthDashboardConnection> connection;
        {
            std::scoped_lock lock(mutex_);
            connection = connection_;
        }
        markFailure(connection);
    }
}

}  // namespace spark
