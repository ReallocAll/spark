#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "core/config/trusted_viewers.h"
#include "core/profiler/profiler.h"
#include "core/stats/statistics_service.h"
#include "selftest_internal.h"

namespace spark::selftest {

bool verifyLiveProfilerWindowStatistics(std::uint64_t worker_tid)
{
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() / "spark-selftest-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerService service(statistics, {}, std::filesystem::temp_directory_path(), {}, {}, {}, false, 10,
                                   "by-pool", "default", trusted_viewers, dispatcher, metadata_provider, notifier);

    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.ignore_sleeping = false;
    std::string error;
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        std::fprintf(stderr, "live profiler windows: profiler start failed: %s\n", error.c_str());
        return false;
    }

    const std::int64_t profile_start = spark::ProfilerServiceTestAccess::startTimeMs(service);
    spark::CpuSnapshot cpu;
    cpu.valid = true;
    cpu.process_ticks_per_second = 100.0;
    cpu.cpu_threads = 2;
    statistics.startAt(0, profile_start, cpu);
    statistics.recordTickAt(5.0, 100);
    statistics.recordTickAt(7.0, 600);
    cpu.process_ticks += 20;
    cpu.system_busy += 20;
    cpu.system_total += 100;
    cpu.wall_ms = 1'000;
    statistics.recordCpuSnapshot(cpu);
    statistics.recordTickAt(6.0, 1'100);
    statistics.recordTickAt(8.0, 1'600);
    cpu.process_ticks += 40;
    cpu.system_busy += 40;
    cpu.system_total += 100;
    cpu.wall_ms = 2'000;
    statistics.recordCpuSnapshot(cpu);

    std::string live_data;
    for (int update = 0; update < 3; ++update) {
        live_data = spark::ProfilerServiceTestAccess::buildLiveSamplerData(service, profile_start + 2'000 + update);
        if (live_data.empty()) {
            std::fprintf(stderr, "live profiler windows: repeated live export failed\n");
            spark::ProfilerServiceTestAccess::cancel(service);
            return false;
        }
    }
    const std::uint64_t samples_after_exports = spark::ProfilerServiceTestAccess::sampleCount(service);
    if (!waitForCondition(
            [&service, samples_after_exports]() {
                return spark::ProfilerServiceTestAccess::sampleCount(service) > samples_after_exports;
            },
            std::chrono::seconds(2))) {
        std::fprintf(stderr, "live profiler windows: sampler did not resume after repeated exports\n");
        spark::ProfilerServiceTestAccess::cancel(service);
        return false;
    }
    spark::ProfilerServiceTestAccess::cancel(service);

    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        std::fprintf(stderr, "live profiler windows: new session failed after repeated exports\n");
        return false;
    }
    spark::ProfilerServiceTestAccess::cancel(service);

    ProtoField time_windows;
    if (!findProtoField(live_data, 6, time_windows) || time_windows.wire_type != 2) {
        std::fprintf(stderr, "live profiler windows: time_windows was absent\n");
        return false;
    }
    std::size_t windows_offset = 0;
    std::vector<std::uint64_t> windows;
    std::uint64_t window = 0;
    while (windows_offset < time_windows.bytes.size() && readProtoVarint(time_windows.bytes, windows_offset, window)) {
        windows.push_back(window);
    }

    std::vector<std::uint64_t> statistic_windows;
    bool has_graph_fields = false;
    for (std::size_t occurrence = 0;; ++occurrence) {
        ProtoField entry;
        if (!findProtoField(live_data, 7, entry, occurrence)) {
            break;
        }
        ProtoField key;
        ProtoField value;
        if (entry.wire_type != 2 || !findProtoField(entry.bytes, 1, key) || !findProtoField(entry.bytes, 2, value) ||
            value.wire_type != 2) {
            std::fprintf(stderr, "live profiler windows: malformed time_window_statistics entry\n");
            return false;
        }
        statistic_windows.push_back(key.varint);
        ProtoField tps;
        ProtoField mspt;
        ProtoField cpu_process;
        ProtoField cpu_system;
        has_graph_fields = has_graph_fields ||
                           (findProtoField(value.bytes, 4, tps) && findProtoField(value.bytes, 5, mspt) &&
                            findProtoField(value.bytes, 2, cpu_process) && findProtoField(value.bytes, 3, cpu_system));
    }

    if (windows.empty() || statistic_windows != windows || !has_graph_fields) {
        std::fprintf(stderr,
                     "live profiler windows: expected matching drawable windows "
                     "(windows=%zu statistics=%zu graph-fields=%d)\n",
                     windows.size(), statistic_windows.size(), static_cast<int>(has_graph_fields));
        return false;
    }
    return true;
}

bool verifyLiveExportStopCancel(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

#ifdef __linux__
    constexpr int mode_count = 2;
#else
    constexpr int mode_count = 1;
#endif
    for (int mode = 0; mode < mode_count; ++mode) {
        for (int operation = 0; operation < 2; ++operation) {
            spark::Profiler profiler;
            spark::ProfilerOptions options;
            options.interval_ms = 1;
            options.alloc = mode == 1;
            options.allocation_interval_bytes = 1;
            std::string error;
            if (!profiler.start(options, worker_tid, error)) {
                return false;
            }

            std::mutex mutex;
            std::condition_variable cv;
            bool paused = false;
            bool release = false;
            bool stop_requested = false;
            spark::ProfilerTestAccess::setLiveExportPausedHook(profiler, [&] {
                std::unique_lock lock(mutex);
                paused = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
            });
            spark::ProfilerTestAccess::setStopRequestedHook(profiler, [&] {
                std::scoped_lock lock(mutex);
                stop_requested = true;
                cv.notify_all();
            });

            std::atomic<bool> live_ok{false};
            const std::uint64_t service_starts = spark::ProfilerTestAccess::samplerServiceStarts(profiler);
            std::thread live([&] {
                try {
                    live_ok.store(!profiler.liveExport({}).empty());
                }
                catch (...) {
                    live_ok.store(false);
                }
            });
            {
                std::unique_lock lock(mutex);
                if (!cv.wait_for(lock, 2s, [&] { return paused; })) {
                    release = true;
                    cv.notify_all();
                    live.join();
                    return false;
                }
            }

            std::atomic<bool> finish_ok{false};
            std::thread finish([&] {
                std::string finish_error;
                if (operation == 0) {
                    if (profiler.stopSampling(finish_error)) {
                        const std::string data = profiler.exportData({});
                        finish_ok.store(!data.empty() && profiler.resumePersistentAllocationCounting(finish_error));
                    }
                }
                else {
                    finish_ok.store(profiler.cancel(finish_error));
                }
            });
            {
                std::unique_lock lock(mutex);
                if (!cv.wait_for(lock, 2s, [&] { return stop_requested; })) {
                    release = true;
                    cv.notify_all();
                    live.join();
                    finish.join();
                    return false;
                }
                release = true;
            }
            cv.notify_all();
            live.join();
            finish.join();
            spark::ProfilerTestAccess::setLiveExportPausedHook(profiler, {});
            spark::ProfilerTestAccess::setStopRequestedHook(profiler, {});

            if (!live_ok.load() || !finish_ok.load() || profiler.running() ||
                spark::ProfilerTestAccess::samplerServiceStarts(profiler) != service_starts ||
                spark::ProfilerTestAccess::backendRunning(profiler)) {
                std::fprintf(stderr, "live lifecycle: stopped session was resumed (mode=%d operation=%d)\n", mode,
                             operation);
                return false;
            }
            if (!profiler.start(options, worker_tid, error) || !profiler.cancel(error)) {
                std::fprintf(stderr, "live lifecycle: new session could not start after operation %d\n", operation);
                return false;
            }
        }
    }
    return true;
}

bool verifyLiveExportTimeout(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() / "spark-timeout-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerService service(statistics, {}, std::filesystem::temp_directory_path(), {}, {}, {}, false, 10,
                                   "by-pool", "default", trusted_viewers, dispatcher, metadata_provider, notifier);
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.save_to_file = true;
    std::string error;
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        return false;
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool paused = false;
    bool release = false;
    bool stop_requested = false;
    spark::ProfilerServiceTestAccess::setLiveExportPausedHook(service, [&] {
        std::unique_lock lock(mutex);
        paused = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    spark::ProfilerServiceTestAccess::setStopRequestedHook(service, [&] {
        std::scoped_lock lock(mutex);
        stop_requested = true;
        cv.notify_all();
    });

    std::atomic<bool> live_failed{false};
    const std::uint64_t service_starts = spark::ProfilerServiceTestAccess::samplerServiceStarts(service);
    std::thread live([&] {
        try {
            spark::ProfilerServiceTestAccess::liveExport(service, {});
        }
        catch (...) {
            live_failed.store(true);
        }
    });
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 2s, [&] { return paused; })) {
            release = true;
            cv.notify_all();
            live.join();
            return false;
        }
    }
    spark::ProfilerServiceTestAccess::expire(service);
    std::thread timeout([&] { service.onTick(1.0); });
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 2s, [&] { return stop_requested; })) {
            release = true;
            cv.notify_all();
            live.join();
            timeout.join();
            return false;
        }
        release = true;
    }
    cv.notify_all();
    live.join();
    timeout.join();
    service.shutdown();
    if (live_failed.load() || service.running() ||
        spark::ProfilerServiceTestAccess::samplerServiceStarts(service) != service_starts ||
        spark::ProfilerServiceTestAccess::samplerRunning(service)) {
        std::fprintf(stderr, "live lifecycle: timeout resumed a stopped sampler\n");
        return false;
    }
    spark::ProfilerServiceTestAccess::setLiveExportPausedHook(service, {});
    spark::ProfilerServiceTestAccess::setStopRequestedHook(service, {});
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        std::fprintf(stderr, "live lifecycle: new session failed after timeout\n");
        return false;
    }
    spark::ProfilerServiceTestAccess::cancel(service);
    return true;
}

}  // namespace spark::selftest
