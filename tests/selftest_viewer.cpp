// Offline integration tests for sampling, allocation hooks, and spark serialization;
// no BDS is involved. The default mode writes profile.pb and profile.sparkprofile.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/health/health_command.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "core/command/arguments.h"
#include "core/config/trusted_viewers.h"
#include "native/alloc/allocation_sampler.h"
#include "native/alloc/allocation_thread_filter.h"
#include "native/alloc/byte_sampler.h"
#ifdef __linux__
#include "native/alloc/elf_import_hooks.h"
#endif
#include "core/profiler/profiler.h"
#include "core/stats/executable_hash.h"
#include "core/stats/statistics_service.h"
#include "core/stats/tick_monitor.h"
#include "native/sampler/capture.h"
#include "native/sampler/thread_info.h"
#include "native/sampler/types.h"
#include "native/symbol/symbolicate.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "net/profile_file.h"
#include "proto/sampler_data.h"
#include "selftest_internal.h"
#include "spark_constants.h"

namespace spark::selftest {

bool verifyViewerShutdownDuringLiveExport(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() / "spark-shutdown-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    TestCommandSender sender;
    spark::ProfilerService service(statistics, {}, std::filesystem::temp_directory_path(), {}, {}, {}, false, 10,
                                   "by-pool", "default", trusted_viewers, dispatcher, metadata_provider, notifier);
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    std::string error;
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        return false;
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool paused = false;
    bool release = false;
    spark::ProfilerServiceTestAccess::setLiveExportPausedHook(service, [&] {
        std::unique_lock lock(mutex);
        paused = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    spark::ProfilerServiceTestAccess::setViewerOpenFunction(
        service, [](spark::ViewerSocket &, const spark::ViewerSocket::UploadCallback &upload) {
            upload("channel");
            return std::string();
        });
    service.cmdOpen(sender, spark::Arguments({"open"}, true));
    const std::uint64_t service_starts = spark::ProfilerServiceTestAccess::samplerServiceStarts(service);
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 3s, [&] { return paused; })) {
            return false;
        }
    }
    std::atomic<bool> shutdown_started{false};
    std::thread shutdown([&] {
        shutdown_started.store(true);
        service.shutdown();
    });
    if (!waitForCondition(
            [&] { return shutdown_started.load() && spark::ProfilerServiceTestAccess::stopRequested(service); }, 1s)) {
        return false;
    }
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    cv.notify_all();
    shutdown.join();
    spark::ProfilerServiceTestAccess::setLiveExportPausedHook(service, {});
    if (!service.shutdownBackend(error) || service.running() ||
        spark::ProfilerServiceTestAccess::samplerServiceStarts(service) != service_starts ||
        spark::ProfilerServiceTestAccess::samplerRunning(service)) {
        std::fprintf(stderr, "live lifecycle: viewer shutdown left sampler workers alive\n");
        return false;
    }
    return true;
}

bool verifyViewerDisconnectKeepsProfilerRunning(std::uint64_t worker_tid)
{
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() /
                                               "spark-disconnect-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerService service(statistics, {}, std::filesystem::temp_directory_path(), {}, {}, {}, false, 10,
                                   "by-pool", "default", trusted_viewers, dispatcher, metadata_provider, notifier);
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    std::string error;
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        return false;
    }

    auto viewer = std::make_shared<spark::ViewerSocket>(spark::ViewerSocket::Config{}, spark::Crypto::KeyPair{});
    spark::ViewerSocketTestAccess::markOpen(*viewer);
    spark::ProfilerServiceTestAccess::setViewerSocket(service, viewer);
    spark::ViewerSocketTestAccess::terminate(*viewer, spark::WebSocketClient::TerminationKind::RemoteClose);
    service.onTick(1.0);

    const bool diagnosed = notifier.contains("Live viewer closed: remote endpoint closed the connection");
    const bool healthy = service.running() && spark::ProfilerServiceTestAccess::samplerRunning(service) && diagnosed &&
                         !spark::ProfilerServiceTestAccess::hasViewerSocket(service);
    spark::ProfilerServiceTestAccess::cancel(service);
    if (!healthy) {
        std::fprintf(stderr, "live viewer disconnect stopped the profiler\n");
    }
    return healthy;
}

bool verifyAllocationViewerLifecycle(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() /
                                               "spark-allocation-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    TestCommandSender sender;
    spark::ProfilerService service(statistics, {}, std::filesystem::temp_directory_path(), {}, {}, {}, false, 10,
                                   "by-pool", "default", trusted_viewers, dispatcher, metadata_provider, notifier);
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 1;
    std::string error;
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        return false;
    }

    spark::ProfilerServiceTestAccess::setViewerOpenFunction(
        service, [](spark::ViewerSocket &socket, const spark::ViewerSocket::UploadCallback &) {
            spark::ViewerSocketTestAccess::markOpen(socket);
            return std::string("https://spark.lucko.me/test");
        });
    service.cmdOpen(sender, spark::Arguments({"open"}, true));
    if (!waitForCondition(
            [&] {
                return !spark::ProfilerServiceTestAccess::viewerOpenPending(service) &&
                       spark::ProfilerServiceTestAccess::hasViewerSocket(service);
            },
            3s)) {
        service.shutdown();
        return false;
    }

    std::shared_ptr<spark::ViewerSocket> first = spark::ProfilerServiceTestAccess::viewerSocket(service);
    spark::ViewerSocketTestAccess::terminate(*first, spark::WebSocketClient::TerminationKind::RemoteClose);
    service.onTick(1.0);
    if (!service.running() || !spark::ProfilerServiceTestAccess::backendRunning(service) ||
        spark::ProfilerServiceTestAccess::hasViewerSocket(service)) {
        service.shutdown();
        return false;
    }

    service.cmdOpen(sender, spark::Arguments({"open"}, true));
    if (!waitForCondition(
            [&] {
                return !spark::ProfilerServiceTestAccess::viewerOpenPending(service) &&
                       spark::ProfilerServiceTestAccess::hasViewerSocket(service);
            },
            3s)) {
        service.shutdown();
        return false;
    }
    service.cmdCancel(sender);
    const bool valid = !service.running() && !spark::ProfilerServiceTestAccess::backendRunning(service);
    service.shutdown();
    return valid;
}

bool verifyWorkerExceptionBoundaries(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    spark::Sampler sampler;
    spark::SamplerConfig sampler_config;
    sampler_config.interval_us = 1000;
    sampler.setTarget(worker_tid);
    spark::SamplerTestAccess::setSamplerThreadHook(sampler, [] { throw std::runtime_error("injected"); });
    if (!sampler.start(sampler_config) || !waitForCondition([&] { return !sampler.running(); }, 2s)) {
        return false;
    }
    std::string worker_error;
    if (!sampler.failure(worker_error) || !sampler.stop() || spark::SamplerTestAccess::workersJoinable(sampler)) {
        std::fprintf(stderr, "worker exception: sampler worker was not contained\n");
        return false;
    }
    spark::SamplerTestAccess::setSamplerThreadHook(sampler, {});
    if (!sampler.start(sampler_config) || !sampler.stop()) {
        std::fprintf(stderr, "worker exception: sampler could not restart\n");
        return false;
    }

    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() / "spark-worker-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    TestCommandSender sender;
    spark::ProfilerService service(statistics, {}, std::filesystem::temp_directory_path(), {}, {}, {}, false, 10,
                                   "by-pool", "default", trusted_viewers, dispatcher, metadata_provider, notifier);
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.save_to_file = true;
    std::string error;
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        return false;
    }
    spark::ProfilerServiceTestAccess::setLiveExportPausedHook(
        service, [] { throw std::runtime_error("injected live serializer failure"); });
    spark::ProfilerServiceTestAccess::setViewerOpenFunction(
        service, [](spark::ViewerSocket &, const spark::ViewerSocket::UploadCallback &upload) {
            upload("channel");
            return std::string();
        });
    service.cmdOpen(sender, spark::Arguments({"open"}, true));
    if (!waitForCondition([&] { return !spark::ProfilerServiceTestAccess::viewerOpenPending(service); }, 3s) ||
        !service.running() || !spark::ProfilerServiceTestAccess::samplerRunning(service)) {
        std::fprintf(stderr, "worker exception: viewer failure escaped or left sampler paused\n");
        return false;
    }
    spark::ProfilerServiceTestAccess::setLiveExportPausedHook(service, {});
    spark::ProfilerServiceTestAccess::setViewerOpenFunction(
        service, [](spark::ViewerSocket &, const spark::ViewerSocket::UploadCallback &) { return std::string(); });
    service.cmdOpen(sender, spark::Arguments({"open"}, true));
    if (!waitForCondition([&] { return !spark::ProfilerServiceTestAccess::viewerOpenPending(service); }, 3s)) {
        return false;
    }

    dispatcher.setReject(true);
    service.cmdStop(sender, spark::Arguments({"stop", "--save-to-file"}, true));
    if (!waitForCondition([&] { return spark::ProfilerServiceTestAccess::exportCompletionPending(service); }, 3s)) {
        return false;
    }
    dispatcher.setReject(false);
    service.onTick(1.0);
    if (service.exporting()) {
        std::fprintf(stderr, "worker exception: rejected export completion did not recover\n");
        return false;
    }
    service.shutdown();

    spark::HealthCommand health(statistics, metadata_provider, {}, {}, {}, trusted_viewers, dispatcher, notifier);
    spark::HealthCommandTestAccess::setUploadFunction(health,
                                                      [](const std::string &, const std::string &, const std::string &,
                                                         const std::string &) -> spark::UploadResult { throw 7; });
    health.cmdHealth(sender, spark::Arguments({"health", "--upload"}, true));
    if (!waitForCondition([&] { return !spark::HealthCommandTestAccess::uploading(health); }, 3s)) {
        std::fprintf(stderr, "worker exception: health worker exception escaped\n");
        return false;
    }
    health.shutdown();
    return true;
}

}  // namespace spark::selftest
