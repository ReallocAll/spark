#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "core/config/trusted_viewers.h"
#include "core/stats/statistics_service.h"

namespace spark {

struct ProfilerTestAccess {
    static bool samplerRunning(const Profiler &profiler) { return profiler.sampler_.running(); }
};

struct ProfilerServiceTestAccess {
    static bool start(ProfilerService &service, const ProfilerOptions &options, std::uint64_t main_tid,
                      std::string &error)
    {
        return service.profiler_.start(options, main_tid, error);
    }

    static bool armTimeout(ProfilerService &service, std::int64_t timeout_seconds)
    {
        return service.armProfilerTimeout(timeout_seconds);
    }

    static bool timeoutPending(const ProfilerService &service)
    {
        return service.timeout_completion_pending_.load(std::memory_order_acquire);
    }

    static void cancel(ProfilerService &service)
    {
        service.resetProfilerTimeout();
        service.profiler_.cancel();
    }

    static bool samplerRunning(const ProfilerService &service)
    {
        return ProfilerTestAccess::samplerRunning(service.profiler_);
    }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class TestMetadataProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext & /*ctx*/, std::int64_t /*now_ms*/) override
    {
        server_metadata_calls_.fetch_add(1, std::memory_order_relaxed);
    }
    void gatherWorldMetadata(spark::ExportContext & /*ctx*/) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }

    int serverMetadataCalls() const { return server_metadata_calls_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> server_metadata_calls_{0};
};

class TestNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string & /*sender_name*/, const std::string & /*text*/) override {}
};

std::uint64_t currentThreadId()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

void nativeWorker(std::atomic<bool> &run, std::atomic<std::uint64_t> &thread_id)
{
    thread_id.store(currentThreadId(), std::memory_order_release);
    while (run.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void verifyProfilerTimeoutLifecycle(std::uint64_t worker_tid, const std::filesystem::path &root)
{
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerService service(statistics, {}, root, {}, {}, {}, false, 10, "by-pool", "default", trusted_viewers,
                                   dispatcher, metadata_provider, notifier);
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    std::string error;

    assert(spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error));
    assert(spark::ProfilerServiceTestAccess::armTimeout(service, 1));
    assert(waitFor(
        [&] {
            return spark::ProfilerServiceTestAccess::timeoutPending(service) &&
                   !spark::ProfilerServiceTestAccess::samplerRunning(service);
        },
        3s));
    assert(service.running());
    assert(!service.exporting());
    assert(metadata_provider.serverMetadataCalls() == 0);

    service.onTick(1.0);
    assert(metadata_provider.serverMetadataCalls() != 0);
    assert(!service.running());
    assert(waitFor([&] { return !service.exporting(); }, 3s));

    assert(spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error));
    assert(spark::ProfilerServiceTestAccess::armTimeout(service, 1));
    spark::ProfilerServiceTestAccess::cancel(service);
    std::this_thread::sleep_for(1200ms);
    assert(!service.running());
    assert(!spark::ProfilerServiceTestAccess::timeoutPending(service));

    assert(spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error));
    std::this_thread::sleep_for(1200ms);
    assert(service.running());
    assert(spark::ProfilerServiceTestAccess::samplerRunning(service));
    assert(!spark::ProfilerServiceTestAccess::timeoutPending(service));
    spark::ProfilerServiceTestAccess::cancel(service);
    service.shutdown();
}

}  // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "spark-profiler-timeout-integration-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> worker_tid{0};
    std::thread worker(nativeWorker, std::ref(run), std::ref(worker_tid));
    assert(waitFor([&] { return worker_tid.load(std::memory_order_acquire) != 0; }, 3s));

    verifyProfilerTimeoutLifecycle(worker_tid.load(std::memory_order_acquire), root);

    run.store(false, std::memory_order_release);
    worker.join();
    std::filesystem::remove_all(root, error);
    return 0;
}
