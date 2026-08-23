#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/profiler/profiler_open_orchestrator.h"

namespace spark {

struct ProfilerOpenTestAccess {
    static void setOpenFunction(
        ProfilerOpenOrchestrator &open,
        std::function<std::string(ViewerSocket &, const ViewerSocket::UploadCallback &)> function)
    {
        open.setViewerOpenFunctionForTesting(std::move(function));
    }

    static const std::string &openComment(const ProfilerOpenOrchestrator &open) { return open.openCommentForTesting(); }

    static ExportContext capture(ProfilerOpenOrchestrator &open, std::int64_t now_ms, const std::string &comment)
    {
        return open.captureLiveContext(now_ms, comment);
    }

    static std::string build(ProfilerOpenOrchestrator &open, const ExportContext &context)
    {
        return open.buildLiveSamplerData(context);
    }
};

}  // namespace spark

namespace {

class TestSender final : public spark::CommandSender {
public:
    std::string getName() const override { return "Console"; }
    bool isPlayer() const override { return false; }
    std::vector<std::string> messages;
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &message) override { messages.push_back(message); }
    void errorImpl(const std::string &message) override { errors.push_back(message); }
};

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class TestMetadataProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext &, std::int64_t) override {}
    void gatherWorldMetadata(spark::ExportContext &) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

class TestNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string &, const std::string &) override {}
};

std::uint64_t currentThreadId()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

void testOpenWithoutCompletedProfile()
{
    spark::Profiler profiler;
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() /
                                               "spark-profiler-open-test-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerOpenOrchestrator open(profiler, statistics, {}, {}, {}, {}, trusted_viewers, dispatcher,
                                         metadata_provider, notifier);
    TestSender sender;

    open.cmdOpen(sender, spark::Arguments({"open"}));
    assert(sender.messages.size() == 1);
    assert(sender.messages.front().find("The profiler isn't running!") != std::string::npos);
    assert(!open.viewerSocket());
    open.shutdown();
}

void testOpenCommentReachesLiveMetadata()
{
    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.comment = "start comment";
    std::string error;
    assert(profiler.start(options, currentThreadId(), error));

    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() /
                                               "spark-profiler-open-test-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerOpenOrchestrator open(profiler, statistics, {}, {}, {}, {}, trusted_viewers, dispatcher,
                                         metadata_provider, notifier);

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    spark::ProfilerOpenTestAccess::setOpenFunction(
        open, [&mutex, &cv, &entered, &release](spark::ViewerSocket &, const spark::ViewerSocket::UploadCallback &) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_one();
            cv.wait(lock, [&release] { return release; });
            return std::string();
        });

    TestSender sender;
    open.cmdOpen(sender, spark::Arguments({"open", "--comment", "open comment"}));
    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(3), [&entered] { return entered; }));
    }
    assert(spark::ProfilerOpenTestAccess::openComment(open) == "open comment");

    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const spark::ExportContext context =
        spark::ProfilerOpenTestAccess::capture(open, now_ms, spark::ProfilerOpenTestAccess::openComment(open));
    const std::string data = spark::ProfilerOpenTestAccess::build(open, context);
    assert(data.find("open comment") != std::string::npos);

    open.close();
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    cv.notify_one();
    open.shutdown();
    assert(profiler.cancel(error));
}

}  // namespace

int main()
{
    testOpenWithoutCompletedProfile();
    testOpenCommentReachesLiveMetadata();
    return 0;
}
