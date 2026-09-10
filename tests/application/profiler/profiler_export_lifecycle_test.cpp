#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "application/platform_capabilities.h"
#include "application/profiler/profile_exporter.h"
#include "application/profiler/profiler_service.h"
#include "core/config/trusted_viewers.h"
#include "core/stats/statistics_service.h"

namespace spark {

struct ProfilerLifecycleTestAccess {
    static bool recoveryRetained(const Profiler &profiler)
    {
        return profiler.retain_recovery_journal_on_shutdown_.load(std::memory_order_acquire);
    }

    static void setRecoveryRemove(Profiler &profiler,
                                  std::function<void(const std::filesystem::path &, std::error_code &)> remover)
    {
        profiler.recovery_remove_function_ = std::move(remover);
    }
};

struct ProfileExporterTestAccess {
    using Upload = std::function<UploadResult(const std::string &, const std::string &, const std::string &,
                                              const std::string &, CancellationToken)>;
    using Save = std::function<ProfileFileResult(const std::filesystem::path &, std::string_view, std::int64_t)>;

    static void setUpload(ProfileExporter &exporter, Upload upload) { exporter.upload_function_ = std::move(upload); }
    static void setSave(ProfileExporter &exporter, Save save) { exporter.save_function_ = std::move(save); }
};

struct ProfilerServiceTestAccess {
    using Export = std::function<ProfileExporter::Result(Profiler &, const ExportContext &, bool, CancellationToken)>;

    static Profiler &profiler(ProfilerService &service) { return service.profiler_; }

    static void setExport(ProfilerService &service, Export export_function)
    {
        service.export_function_ = std::move(export_function);
    }

    static void setPostPublicationHook(ProfilerService &service, std::function<void()> hook)
    {
        service.export_post_publication_hook_ = std::move(hook);
    }

    static void setPreparationHook(ProfilerService &service, std::function<void()> hook)
    {
        service.export_job_preparation_hook_ = std::move(hook);
    }

    static void setShutdownBudget(ProfilerService &service, std::chrono::milliseconds timeout)
    {
        service.export_shutdown_timeout_ = timeout;
    }

    static void finish(ProfilerService &service, bool save)
    {
        service.finishProfiler("Alice", true, "player-id", save, "lifecycle test");
    }

    static bool completionPending(const ProfilerService &service)
    {
        return service.export_completion_pending_.load(std::memory_order_acquire);
    }

    static bool workerExited(const ProfilerService &service)
    {
        std::scoped_lock lock(service.export_mutex_);
        return service.export_worker_exited_;
    }

    static bool workerJoinable(const ProfilerService &service)
    {
        std::scoped_lock lock(service.export_mutex_);
        return service.export_thread_.joinable();
    }

    static std::thread::id workerId(const ProfilerService &service)
    {
        std::scoped_lock lock(service.export_mutex_);
        return service.export_thread_.get_id();
    }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override
    {
        ++calls;
        task();
    }

    std::atomic<int> calls{0};
};

class TestMetadata final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext &, std::int64_t) override {}
    void gatherWorldMetadata(spark::ExportContext &) override {}
    std::vector<spark::NativePluginSource> nativePluginSources() override { return {}; }
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

class TestNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string &, const std::string &text) override { messages.push_back(text); }

    std::vector<std::string> messages;
};

struct ServiceFixture {
    explicit ServiceFixture(const std::filesystem::path &root, bool background = false)
        : trusted(root / "trusted-viewers.json"),
          service(statistics, {}, root, {}, "https://viewer/", {}, background, 10, "by-pool", "default", trusted,
                  dispatcher, metadata, notifier)
    {
    }

    spark::StatisticsService statistics;
    TestMetadata metadata;
    TestDispatcher dispatcher;
    TestNotifier notifier;
    spark::TrustedViewersState trusted;
    spark::ProfilerService service;
};

struct BlockingExport {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;

    spark::ProfileExporter::Result operator()(spark::Profiler &, const spark::ExportContext &, bool,
                                              const spark::CancellationToken &)
    {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [this] { return release; });
        spark::ProfileExporter::Result result;
        result.message = "blocked export released";
        return result;
    }

    bool waitEntered()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return entered; });
    }

    void releaseExport()
    {
        std::scoped_lock lock(mutex);
        release = true;
        condition.notify_all();
    }
};

struct PostPublicationGate {
    std::mutex mutex;
    std::condition_variable condition;
    int calls = 0;
    bool release_first = false;
    bool first_done = false;

    void operator()()
    {
        std::unique_lock lock(mutex);
        ++calls;
        if (calls != 1) {
            return;
        }
        condition.notify_all();
        condition.wait(lock, [this] { return release_first; });
        first_done = true;
        condition.notify_all();
    }

    bool waitEntered()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return calls != 0; });
    }

    void release()
    {
        std::scoped_lock lock(mutex);
        release_first = true;
        condition.notify_all();
    }

    bool waitDone()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return first_done; });
    }
};

std::filesystem::path testRoot(std::string_view name)
{
    const auto root = std::filesystem::temp_directory_path() / ("spark_profiler_export_lifecycle_" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    return root;
}

spark::ProfileExporter::Result deliveredResult(spark::ExportOutcome outcome, std::string message)
{
    spark::ProfileExporter::Result result;
    result.outcome = outcome;
    result.message = std::move(message);
    return result;
}

void testBlockedExportShutdownRetry()
{
    BlockingExport blocked;
    ServiceFixture fixture(testRoot("blocked"));
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 30ms);
    spark::ProfilerServiceTestAccess::setExport(fixture.service, std::ref(blocked));
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(blocked.waitEntered());

    std::string error;
    assert(!fixture.service.shutdown(error));
    assert(error.find("5000 milliseconds") != std::string::npos);
    assert(fixture.service.exporting());
    assert(spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    assert(fixture.notifier.messages.empty());

    blocked.releaseExport();
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::workerExited(fixture.service); }, 2s));
    assert(fixture.service.shutdown(error));
    assert(!fixture.service.exporting());
    assert(!spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    assert(fixture.notifier.messages.empty());
}

void testPublishedResultConsumedOnTickAndWorkerReused()
{
    PostPublicationGate publication_gate;
    std::atomic<int> export_calls{0};
    ServiceFixture fixture(testRoot("publish"));
    spark::ProfilerServiceTestAccess::setExport(
        fixture.service,
        [&export_calls](spark::Profiler &, const spark::ExportContext &, bool, const spark::CancellationToken &) {
            export_calls.fetch_add(1, std::memory_order_release);
            return deliveredResult(spark::ExportOutcome::Saved, "saved");
        });
    spark::ProfilerServiceTestAccess::setPostPublicationHook(fixture.service, std::ref(publication_gate));

    spark::ProfilerServiceTestAccess::finish(fixture.service, true);
    assert(publication_gate.waitEntered());
    assert(spark::ProfilerServiceTestAccess::completionPending(fixture.service));
    const std::thread::id worker_id = spark::ProfilerServiceTestAccess::workerId(fixture.service);

    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.size() == 2);
    assert(!fixture.service.exporting());
    assert(fixture.dispatcher.calls.load() == 0);
    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.size() == 2);

    publication_gate.release();
    assert(publication_gate.waitDone());
    assert(waitFor([&] { return export_calls.load(std::memory_order_acquire) == 1; }, 2s));

    spark::ProfilerServiceTestAccess::finish(fixture.service, true);
    assert(waitFor([&] { return export_calls.load(std::memory_order_acquire) == 2; }, 2s));
    assert(spark::ProfilerServiceTestAccess::workerId(fixture.service) == worker_id);
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::completionPending(fixture.service); }, 2s));
    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.size() == 4);

    std::string error;
    assert(fixture.service.shutdown(error));
}

void testPreparationThrowPublishesTerminalFailure()
{
    ServiceFixture fixture(testRoot("throw"));
    spark::ProfilerServiceTestAccess::setPreparationHook(fixture.service, [] { throw std::runtime_error("prepare"); });
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::completionPending(fixture.service); }, 2s));
    fixture.service.onTick(0.0);
    assert(!fixture.service.exporting());
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::workerExited(fixture.service); }, 2s));
    assert(spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    assert(fixture.notifier.messages.size() == 2);

    spark::ProfilerServiceTestAccess::setPreparationHook(fixture.service, {});
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(!spark::ProfilerServiceTestAccess::completionPending(fixture.service));
    assert(!fixture.service.exporting());
    assert(spark::ProfilerServiceTestAccess::workerExited(fixture.service));
    assert(spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    assert(fixture.notifier.messages.size() == 3);
    assert(fixture.notifier.messages.back().find("worker has exited") != std::string::npos);

    std::string error;
    assert(fixture.service.shutdown(error));
}

void testShutdownConsumesResultSilentlyAndRetainsJournal()
{
    const auto root = testRoot("silent-shutdown");
    ServiceFixture fixture(root);
    const auto journal = root / "journal";
    const auto sentinel = journal / "sentinel";
    std::filesystem::create_directories(journal);
    {
        std::ofstream output(sentinel, std::ios::binary);
        output << "recovery-sentinel";
    }
    assert(std::filesystem::exists(sentinel));
    fixture.service.setRecoveryDirectory(journal);
    spark::ProfilerServiceTestAccess::setExport(
        fixture.service, [](spark::Profiler &, const spark::ExportContext &, bool, const spark::CancellationToken &) {
            return deliveredResult(spark::ExportOutcome::Saved, "saved");
        });
    spark::ProfilerServiceTestAccess::finish(fixture.service, true);
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::completionPending(fixture.service); }, 2s));

    std::string error;
    assert(fixture.service.shutdown(error));
    assert(fixture.service.shutdownBackend(error));
    assert(fixture.notifier.messages.empty());
    assert(spark::ProfilerLifecycleTestAccess::recoveryRetained(
        spark::ProfilerServiceTestAccess::profiler(fixture.service)));
    assert(std::filesystem::exists(sentinel));
    std::ifstream input(sentinel, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(contents == "recovery-sentinel");
    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.empty());
}

void testExporterFallbackCancellationStatus()
{
    const auto root = testRoot("exporter");
    std::atomic<int> uploads{0};
    std::atomic<int> saves{0};

    spark::ProfileExporter exporter(root, "https://bytebin/", "https://viewer/");
    spark::Profiler profiler;
    spark::ProfileExporterTestAccess::setUpload(exporter, [&uploads](const std::string &, const std::string &,
                                                                     const std::string &, const std::string &,
                                                                     const spark::CancellationToken &) {
        ++uploads;
        return spark::UploadResult{.ok = false, .key = {}, .error = "offline"};
    });
    spark::ProfileExporterTestAccess::setSave(
        exporter, [&saves](const std::filesystem::path &path, std::string_view, std::int64_t) {
            ++saves;
            return spark::ProfileFileResult{.ok = true, .path = path / "fallback.sparkprofile", .error = {}};
        });
    auto result = exporter.exportProfile(profiler, {}, false);
    assert(result.outcome == spark::ExportOutcome::Saved);
    assert(uploads.load() == 1);
    assert(saves.load() == 1);
    assert(!result.retain_recovery_journal);

    spark::CancellationSource during_upload;
    uploads.store(0);
    saves.store(0);
    spark::ProfileExporterTestAccess::setUpload(
        exporter, [&during_upload, &uploads](const std::string &, const std::string &, const std::string &,
                                             const std::string &, const spark::CancellationToken &) {
            ++uploads;
            during_upload.requestStop();
            return spark::UploadResult{.ok = false, .key = {}, .error = "offline"};
        });
    result = exporter.exportProfile(profiler, {}, false, during_upload.token());
    assert(result.outcome == spark::ExportOutcome::Failed);
    assert(result.retain_recovery_journal);
    assert(uploads.load() == 1);
    assert(saves.load() == 0);

    spark::CancellationSource during_fallback;
    uploads.store(0);
    saves.store(0);
    spark::ProfileExporterTestAccess::setUpload(exporter, [&uploads](const std::string &, const std::string &,
                                                                     const std::string &, const std::string &,
                                                                     const spark::CancellationToken &) {
        ++uploads;
        return spark::UploadResult{.ok = false, .key = {}, .error = "offline"};
    });
    spark::ProfileExporterTestAccess::setSave(
        exporter, [&during_fallback, &saves](const std::filesystem::path &path, std::string_view, std::int64_t) {
            ++saves;
            during_fallback.requestStop();
            return spark::ProfileFileResult{.ok = true, .path = path / "fallback.sparkprofile", .error = {}};
        });
    result = exporter.exportProfile(profiler, {}, false, during_fallback.token());
    assert(result.outcome == spark::ExportOutcome::Saved);
    assert(result.retain_recovery_journal);
    assert(uploads.load() == 1);
    assert(saves.load() == 1);
}

void testSuccessfulExportReportsCleanupWarning()
{
    const auto root = testRoot("cleanup-warning");
    std::filesystem::create_directories(root / "journal");
    ServiceFixture fixture(root);
    fixture.service.setRecoveryDirectory(root / "journal");
    spark::ProfilerServiceTestAccess::setExport(
        fixture.service, [](spark::Profiler &, const spark::ExportContext &, bool, const spark::CancellationToken &) {
            return deliveredResult(spark::ExportOutcome::Saved, "saved");
        });
    spark::ProfilerLifecycleTestAccess::setRecoveryRemove(spark::ProfilerServiceTestAccess::profiler(fixture.service),
                                                          [](const std::filesystem::path &, std::error_code &error) {
                                                              error =
                                                                  std::make_error_code(std::errc::permission_denied);
                                                          });

    spark::ProfilerServiceTestAccess::finish(fixture.service, true);
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::completionPending(fixture.service); }, 2s));
    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.size() == 2);
    assert(fixture.notifier.messages.back().find("Recovery journal retained") != std::string::npos);
    assert(spark::ProfilerLifecycleTestAccess::recoveryRetained(
        spark::ProfilerServiceTestAccess::profiler(fixture.service)));

    std::string error;
    assert(fixture.service.shutdown(error));
}

[[noreturn]] void failClosedTerminate() noexcept
{
    std::_Exit(91);
}

int runDestructorFailureChild()
{
    std::set_terminate(failClosedTerminate);
    BlockingExport blocked;
    ServiceFixture fixture(testRoot("destructor"));
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 20ms);
    spark::ProfilerServiceTestAccess::setExport(fixture.service, std::ref(blocked));
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(blocked.waitEntered());
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--destructor-fail-closed") {
        return runDestructorFailureChild();
    }
    testBlockedExportShutdownRetry();
    testPublishedResultConsumedOnTickAndWorkerReused();
    testPreparationThrowPublishesTerminalFailure();
    testShutdownConsumesResultSilentlyAndRetainsJournal();
    testExporterFallbackCancellationStatus();
    testSuccessfulExportReportsCleanupWarning();
    return 0;
}
