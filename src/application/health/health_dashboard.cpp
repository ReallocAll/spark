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

}  // namespace spark
