#include <cassert>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include "application/health/health_command.h"

namespace spark {

struct HealthDashboardTestAccess {
    static void setGeneration(HealthDashboard &dashboard, std::uint64_t generation)
    {
        dashboard.generation_ = generation;
    }
};

struct HealthCommandTestAccess {
    static void prepareUpload(HealthCommand &command)
    {
        command.upload_result_ = UploadResult{.ok = true, .key = "health-key"};
        command.upload_sender_ = "Alice";
        command.upload_sender_is_player_ = true;
        command.uploading_.store(true);
    }

    static void announceUpload(HealthCommand &command) { command.announceHealthUpload(); }

    static void prepareStaleCompletion(HealthCommand &command)
    {
        command.accepted_dashboard_generation_ = 2;
        command.dashboard_sender_ = "NewSender";
        HealthDashboardTestAccess::setGeneration(*command.dashboard_, 2);
    }

    static void prepareCurrentFailure(HealthCommand &command)
    {
        command.accepted_dashboard_generation_ = 2;
        command.dashboard_sender_ = "CurrentSender";
        HealthDashboardTestAccess::setGeneration(*command.dashboard_, 2);
    }

    static void complete(HealthCommand &command, HealthDashboard::OpenResult result)
    {
        command.completeHealthDashboard(std::move(result));
    }

    static bool uploading(const HealthCommand &command) { return command.uploading_.load(); }
    static const std::string &dashboardSender(const HealthCommand &command) { return command.dashboard_sender_; }
};

}  // namespace spark

namespace {

class Metadata final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext &, std::int64_t) override {}
    void gatherWorldMetadata(spark::ExportContext &) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

class Dispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class ThrowingNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string &, const std::string &) override { throw std::runtime_error("notify failed"); }
};

void test_upload_state_clears_before_throwing_notification()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-failure-viewers.json");
    spark::HealthCommand command(statistics, metadata, {}, "https://viewer/", {}, trusted, dispatcher, notifier);

    spark::HealthCommandTestAccess::prepareUpload(command);
    spark::HealthCommandTestAccess::announceUpload(command);
    assert(!spark::HealthCommandTestAccess::uploading(command));
}

void test_stale_dashboard_completion_is_ignored()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-stale-viewers.json");
    spark::HealthCommand command(statistics, metadata, {}, {}, {}, trusted, dispatcher, notifier);

    spark::HealthCommandTestAccess::prepareStaleCompletion(command);
    spark::HealthDashboard::OpenResult stale;
    stale.generation = 1;
    stale.sender_name = "OldSender";
    stale.ok = true;
    stale.url = "https://old/";
    spark::HealthCommandTestAccess::complete(command, std::move(stale));
    assert(spark::HealthCommandTestAccess::dashboardSender(command) == "NewSender");
}

void test_current_failed_completion_clears_state_when_notification_throws()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-health-current-failure.json");
    spark::HealthCommand command(statistics, metadata, {}, {}, {}, trusted, dispatcher, notifier);

    spark::HealthCommandTestAccess::prepareCurrentFailure(command);
    spark::HealthDashboard::OpenResult current;
    current.generation = 2;
    current.sender_name = "CurrentSender";
    current.ok = false;
    spark::HealthCommandTestAccess::complete(command, std::move(current));
    assert(spark::HealthCommandTestAccess::dashboardSender(command).empty());
}

}  // namespace

int main()
{
    test_upload_state_clears_before_throwing_notification();
    test_stale_dashboard_completion_is_ignored();
    test_current_failed_completion_clears_state_when_notification_throws();
    return 0;
}
