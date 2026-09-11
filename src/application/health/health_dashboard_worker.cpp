#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

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

void HealthDashboard::requestStop() noexcept
{
    std::shared_ptr<HealthDashboardConnection> connection;
    try {
        {
            std::scoped_lock completion_lock(completion_mutex_);
            callbacks_enabled_ = false;
        }
        {
            std::scoped_lock lock(mutex_);
            if (!stopping_) {
                stopping_ = true;
                ++generation_;
            }
            cancellation_source_.requestStop();
            work_.reset();
            open_pending_ = false;
            open_ = false;
            connection = connection_;
        }
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
        if (connection) {
            connection->requestStop();
        }
    }
    catch (...) {
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
    }
}

bool HealthDashboard::shutdownWithin(std::chrono::milliseconds timeout)
{
    return shutdownUntil(std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero()));
}

bool HealthDashboard::shutdownUntil(std::chrono::steady_clock::time_point deadline)
{
    requestStop();
    if (worker_.isCurrentThread()) {
        return false;
    }

    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    if (lifecycle_active_) {
        if (!lifecycle_cv_.wait_until(lifecycle_lock, deadline, [this] { return !lifecycle_active_; })) {
            return false;
        }
    }
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_complete_) {
            return true;
        }
    }
    lifecycle_active_ = true;
    lifecycle_lock.unlock();

    const auto finish_lifecycle = [this] {
        {
            std::scoped_lock lock(lifecycle_mutex_);
            lifecycle_active_ = false;
        }
        lifecycle_cv_.notify_all();
    };

    requestStop();

    lifecycle_lock.lock();
    if (!lifecycle_cv_.wait_until(lifecycle_lock, deadline, [this] { return open_calls_active_ == 0; })) {
        lifecycle_lock.unlock();
        finish_lifecycle();
        return false;
    }
    lifecycle_lock.unlock();

    if (!worker_.reapUntil(deadline)) {
        finish_lifecycle();
        return false;
    }

    std::shared_ptr<HealthDashboardConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        connection = connection_;
    }
    if (connection) {
        if (!connection->closeUntil(deadline)) {
            finish_lifecycle();
            return false;
        }
    }

    {
        std::scoped_lock lock(mutex_);
        if (connection_ == connection) {
            connection_.reset();
        }
        work_.reset();
        work_active_ = false;
        open_pending_ = false;
        open_ = false;
        shutdown_complete_ = true;
    }
    finish_lifecycle();
    return true;
}

void HealthDashboard::shutdown()
{
    static_cast<void>(shutdownWithin(kDefaultShutdownBudget));
}

bool HealthDashboard::startWorker()
{
    {
        std::scoped_lock lock(lifecycle_mutex_);
        if (lifecycle_active_) {
            return false;
        }
        lifecycle_active_ = true;
    }
    struct LifecycleGuard {
        HealthDashboard &dashboard;
        ~LifecycleGuard()
        {
            {
                std::scoped_lock lock(dashboard.lifecycle_mutex_);
                dashboard.lifecycle_active_ = false;
            }
            dashboard.lifecycle_cv_.notify_all();
        }
    } guard{*this};
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || shutdown_complete_) {
            return false;
        }
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }
    }
    if (!worker_.reapUntil(std::chrono::steady_clock::now())) {
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            return false;
        }
        running_.store(true, std::memory_order_release);
    }
    {
        std::scoped_lock lock(worker_exit_mutex_);
        worker_exited_ = false;
    }
    if (!worker_.start([this] { run(); })) {
        running_.store(false, std::memory_order_release);
        signalWorkerExit();
        return false;
    }
    return true;
}

void HealthDashboard::signalWorkerExit() noexcept
{
    {
        std::scoped_lock lock(worker_exit_mutex_);
        worker_exited_ = true;
    }
    worker_exit_cv_.notify_all();
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
            cancellation_source_.requestStop();
            ++generation_;
            open_ = false;
            open_pending_ = false;
            work_.reset();
        }
    }
    if (connection) {
        connection->requestStop();
    }
    cv_.notify_all();
}

void HealthDashboard::completeOpen(OpenResult result, const std::shared_ptr<HealthDashboardConnection> &connection,
                                   std::int64_t initial_time_ms)
{
    bool current = false;
    bool stopping = false;
    {
        std::scoped_lock lock(mutex_);
        stopping = stopping_;
        current = !stopping_ && result.generation == generation_ && open_pending_ &&
                  (!connection || connection_ == connection);
    }
    if (connection && (!current || !result.ok)) {
        connection->requestStop();
        if (!stopping && connection->closeWithin(kBackgroundCloseBudget)) {
            std::scoped_lock lock(mutex_);
            if (connection_ == connection) {
                connection_.reset();
            }
        }
    }
    result.completed = true;
    {
        std::scoped_lock lock(mutex_);
        current = !stopping_ && result.generation == generation_ && open_pending_;
        work_active_ = false;
        if (current) {
            open_pending_ = false;
            open_ = result.ok;
            if (result.ok) {
                last_update_ms_ = initial_time_ms;
            }
        }
    }
    if (!current) {
        return;
    }
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
        if (stopping_) {
            return;
        }
    }
    if (completion) {
        try {
            completion(std::move(result));
        }
        catch (...) {
            failed_.store(true, std::memory_order_release);
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

                if (work.cancellation.stopRequested()) {
                    std::scoped_lock lock(mutex_);
                    work_active_ = false;
                    continue;
                }

                std::shared_ptr<HealthDashboardConnection> connection;
                {
                    std::scoped_lock lock(mutex_);
                    connection = connection_;
                }
                if (connection) {
                    connection->requestStop();
                    if (!connection->closeWithin(kBackgroundCloseBudget)) {
                        result.error = "previous health dashboard still closing; retry";
                        completeOpen(std::move(result), {}, work.data.generated_time_ms);
                        continue;
                    }
                    {
                        std::scoped_lock lock(mutex_);
                        connection_.reset();
                    }
                    connection.reset();
                }
                if (work.cancellation.stopRequested()) {
                    std::scoped_lock lock(mutex_);
                    work_active_ = false;
                    continue;
                }
                try {
                    std::unique_ptr<HealthDashboardConnection> created = connection_factory_();
                    if (created) {
                        connection = std::shared_ptr<HealthDashboardConnection>(std::move(created));
                        std::scoped_lock lock(mutex_);
                        connection_ = connection;
                    }
                }
                catch (const std::exception &error) {
                    result.error =
                        std::string("health dashboard connection creation failed: ") + exceptionMessage(error);
                }
                catch (...) {
                    result.error = "health dashboard connection creation failed";
                }

                if (work.cancellation.stopRequested()) {
                    if (connection) {
                        connection->requestStop();
                    }
                    std::scoped_lock lock(mutex_);
                    work_active_ = false;
                    continue;
                }
                if (!connection) {
                    if (result.error.empty()) {
                        result.error = "health dashboard connection factory returned null";
                    }
                    completeOpen(std::move(result), {}, work.data.generated_time_ms);
                    continue;
                }

                try {
                    connection->setIsKeyTrustedCallback(is_key_trusted_);
                }
                catch (const std::exception &error) {
                    result.error = std::string("health dashboard connection setup failed: ") + exceptionMessage(error);
                }
                catch (...) {
                    result.error = "health dashboard connection setup failed";
                }
                if (!result.error.empty()) {
                    completeOpen(std::move(result), connection, work.data.generated_time_ms);
                    continue;
                }
                if (work.cancellation.stopRequested()) {
                    connection->requestStop();
                    std::scoped_lock lock(mutex_);
                    work_active_ = false;
                    continue;
                }

                bool admitted = false;
                {
                    std::scoped_lock lock(mutex_);
                    admitted =
                        !stopping_ && work.generation == generation_ && open_pending_ && connection_ == connection;
                }
                if (!admitted || work.cancellation.stopRequested()) {
                    connection->requestStop();
                    std::scoped_lock lock(mutex_);
                    work_active_ = false;
                    continue;
                }

                UploadResult upload_result;
                bool upload_called = false;
                try {
                    const std::string url = connection->open(
                        [&](const CancellationToken &) {
                            if (work.cancellation.stopRequested()) {
                                return UploadResult{.error = "health dashboard upload cancelled"};
                            }
                            HealthData upload_data = work.data;
                            upload_data.channel_info = connection->channelInfo();
                            if (work.cancellation.stopRequested()) {
                                return UploadResult{.error = "health dashboard upload cancelled"};
                            }
                            upload_called = true;
                            if (!initial_upload_) {
                                return UploadResult{.error = "health dashboard upload is not configured"};
                            }
                            upload_result = initial_upload_(upload_data, work.cancellation);
                            return upload_result;
                        },
                        work.cancellation);
                    if (work.cancellation.stopRequested()) {
                        result.error = "health dashboard open cancelled";
                    }
                    else if (!upload_called) {
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
                completeOpen(std::move(result), connection, work.data.generated_time_ms);
                continue;
            }

            if (work.cancellation.stopRequested()) {
                std::scoped_lock lock(mutex_);
                work_active_ = false;
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
                attempted = !stopping_ && !work.cancellation.stopRequested() && work.generation == generation_ &&
                            connection_ == work.connection && open_;
            }
            if (attempted) {
                sent = work.connection->sendStatistics(platform, system, metrics);
            }
            bool current = false;
            {
                std::scoped_lock lock(mutex_);
                current = !stopping_ && work.generation == generation_ && connection_ == work.connection;
                work_active_ = false;
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
            work_active_ = false;
        }
        markFailure(connection);
    }
    signalWorkerExit();
}

}  // namespace spark
