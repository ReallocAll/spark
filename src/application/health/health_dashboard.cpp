#include "application/health/health_dashboard.h"

#include <exception>
#include <utility>

#include "proto/metrics_proto.h"
#include "proto/statistics_proto.h"

namespace spark {

namespace {

std::string exceptionMessage(const std::exception &error)
{
    return error.what();
}

}  // namespace

HealthDashboard::HealthDashboard(ConnectionFactory connection_factory, InitialUpload initial_upload,
                                 IsKeyTrustedCallback is_key_trusted, CompletionCallback completion)
    : connection_factory_(std::move(connection_factory)), initial_upload_(std::move(initial_upload)),
      is_key_trusted_(std::move(is_key_trusted)), completion_(std::move(completion))
{
}

HealthDashboard::~HealthDashboard()
{
    shutdown();
}

HealthDashboard::OpenResult HealthDashboard::open(HealthData initial, const std::string &sender_name)
{
    OpenResult result;
    result.sender_name = sender_name;

    {
        std::scoped_lock lock(mutex_);
        if (open_ || open_pending_ || work_active_) {
            result.error = "health dashboard is already open or opening";
            return result;
        }
    }

    {
        std::scoped_lock lock(completion_mutex_);
        callbacks_enabled_ = true;
    }

    if (!startWorker()) {
        result.error = "health dashboard worker failed to start";
        return result;
    }

    std::unique_ptr<HealthDashboardConnection> created;
    try {
        if (!connection_factory_) {
            result.error = "health dashboard connection factory is not configured";
            return result;
        }
        created = connection_factory_();
    }
    catch (const std::exception &error) {
        result.error = std::string("health dashboard connection creation failed: ") + exceptionMessage(error);
        return result;
    }
    catch (...) {
        result.error = "health dashboard connection creation failed";
        return result;
    }
    if (!created) {
        result.error = "health dashboard connection factory returned null";
        return result;
    }

    auto connection = std::shared_ptr<HealthDashboardConnection>(std::move(created));
    try {
        connection->setIsKeyTrustedCallback(is_key_trusted_);
    }
    catch (const std::exception &error) {
        result.error = std::string("health dashboard connection setup failed: ") + exceptionMessage(error);
        connection->close();
        return result;
    }
    catch (...) {
        result.error = "health dashboard connection setup failed";
        connection->close();
        return result;
    }

    bool reject = false;
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || open_ || open_pending_ || work_active_) {
            result.error = "health dashboard is already open or stopping";
            reject = true;
        }
        else {
            ++generation_;
            result.accepted = true;
            result.generation = generation_;
            connection_ = connection;
            work_ = WorkItem{.type = WorkType::Open,
                             .data = std::move(initial),
                             .connection = connection,
                             .generation = generation_,
                             .sender_name = sender_name};
            open_pending_ = true;
        }
    }
    if (reject) {
        connection->close();
        return result;
    }
    cv_.notify_one();
    return result;
}

bool HealthDashboard::updateDue(std::int64_t now_ms) const
{
    std::shared_ptr<HealthDashboardConnection> connection;
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (!open_ || !connection_ || work_ || work_active_) {
            return false;
        }
        connection = connection_;
        generation = generation_;
    }
    if (!connection->isOpen() || !connection->hasClient()) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    return open_ && connection_ == connection && generation_ == generation && !work_ && !work_active_ &&
           now_ms >= last_update_ms_ && now_ms - last_update_ms_ >= 10000;
}

bool HealthDashboard::enqueueUpdate(HealthData snapshot, std::int64_t now_ms)
{
    std::shared_ptr<HealthDashboardConnection> connection;
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (!open_ || !connection_ || work_ || work_active_ || now_ms < last_update_ms_ ||
            now_ms - last_update_ms_ < 10000) {
            return false;
        }
        connection = connection_;
        generation = generation_;
    }
    if (!connection->isOpen() || !connection->hasClient()) {
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        if (!open_ || connection_ != connection || generation_ != generation || work_ || work_active_ ||
            now_ms < last_update_ms_ || now_ms - last_update_ms_ < 10000) {
            return false;
        }
        work_ = WorkItem{.type = WorkType::Update,
                         .data = std::move(snapshot),
                         .connection = connection,
                         .generation = generation,
                         .sender_name = {}};
        last_update_ms_ = now_ms;
    }
    cv_.notify_one();
    return true;
}

void HealthDashboard::tick()
{
    std::shared_ptr<HealthDashboardConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        if (!open_ || !connection_) {
            return;
        }
        connection = connection_;
    }

    bool alive = false;
    try {
        alive = connection->tick();
    }
    catch (...) {
        alive = false;
    }
    if (!alive) {
        markFailure(connection);
    }
}

void HealthDashboard::shutdown()
{
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
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
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
}

bool HealthDashboard::isOpen() const
{
    std::shared_ptr<HealthDashboardConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        if (!open_ || !connection_) {
            return false;
        }
        connection = connection_;
    }
    const bool connected = connection->isOpen();
    std::scoped_lock lock(mutex_);
    return connected && open_ && connection_ == connection;
}

bool HealthDashboard::openPending() const
{
    std::scoped_lock lock(mutex_);
    return open_pending_;
}

std::uint64_t HealthDashboard::generation() const
{
    std::scoped_lock lock(mutex_);
    return generation_;
}

std::vector<std::uint8_t> HealthDashboard::pendingKey(const std::string &client_id) const
{
    std::shared_ptr<HealthDashboardConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        connection = connection_;
    }
    if (!connection) {
        return {};
    }
    {
        std::scoped_lock lock(mutex_);
        if (connection_ != connection) {
            return {};
        }
    }
    return connection->pendingKey(client_id);
}

void HealthDashboard::sendClientTrusted(const std::string &client_id)
{
    std::shared_ptr<HealthDashboardConnection> connection;
    {
        std::scoped_lock lock(mutex_);
        connection = connection_;
    }
    if (connection) {
        {
            std::scoped_lock lock(mutex_);
            if (connection_ != connection) {
                return;
            }
        }
        connection->sendClientTrusted(client_id);
    }
}

bool HealthDashboard::startWorker()
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
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
    return true;
}

void HealthDashboard::stopWorker()
{
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
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
    std::unique_lock completion_lock(completion_mutex_);
    if (!callbacks_enabled_) {
        return;
    }
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || result.generation != generation_) {
            return;
        }
    }
    if (completion_) {
        try {
            completion_(std::move(result));
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
