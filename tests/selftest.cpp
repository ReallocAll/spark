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

int main(int argc, char **argv)
{
    using namespace std::chrono_literals;

    // Diagnostic: resolve a spread of addresses in a given binary to reproduce
    // symbolication crashes (e.g. the stripped bedrock_server) offline.
    if (argc > 1 && std::string(argv[1]) == "--probe") {
        std::string path = argc > 2 ? argv[2] : "";
        spark::ModuleTable modules;
        spark::ModuleId mid = modules.intern(path);
        std::vector<spark::FrameKey> keys;
        for (std::uint64_t rva = 0x100000; rva < 0x8000000; rva += 0x20000) {
            spark::FrameKey k;
            k.module = mid;
            k.rva = rva;
            keys.push_back(k);
        }
        std::fprintf(stderr, "probe: resolving %zu frames from %s\n", keys.size(), path.c_str());
        auto resolved = spark::resolveFrames(modules, keys);
        std::size_t named = 0;
        for (auto &[k, v] : resolved) {
            if (!v.method_name.starts_with("0x")) {
                ++named;
            }
        }
        std::fprintf(stderr, "probe: resolved=%zu named=%zu (no crash)\n", resolved.size(), named);
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "--allocation-only") {
#ifdef __linux__
        if (!spark::selftest::verifyAllocationLifecycle()) {
            std::fprintf(stderr, "allocation-only: lifecycle test failed\n");
            return 1;
        }
        if (!spark::selftest::verifyAllocationThreadSelection()) {
            std::fprintf(stderr, "allocation-only: thread selection test failed\n");
            return 1;
        }
        if (!spark::selftest::verifyProcessWideAllocationSampling()) {
            std::fprintf(stderr, "allocation-only: process-wide test failed\n");
            return 1;
        }
        if (!spark::selftest::verifyAllocationContentionPolicy()) {
            std::fprintf(stderr, "allocation-only: contention policy test failed\n");
            return 1;
        }
        if (!spark::selftest::verifyAllocationResourcePressure()) {
            std::fprintf(stderr, "allocation-only: resource pressure test failed\n");
            return 1;
        }
        return 0;
#else
        return 0;
#endif
    }

    if (argc > 1 && std::string(argv[1]) == "--statistics-only") {
        return spark::selftest::verifyStatisticsService() && spark::selftest::verifySystemResourceStats() &&
                       spark::selftest::verifyWorldGaugeStatistics() &&
                       spark::selftest::verifyWorldGaugeAbsentWhenNotRecorded()
                 ? 0
                 : 1;
    }

    int seconds = 4;
    bool upload = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--upload") {
            upload = true;
        }
        else if (a.starts_with("--seconds=")) {
            const std::string_view value(a.c_str() + 10);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
                std::fprintf(stderr, "invalid --seconds value\n");
                return 1;
            }
        }
    }

    std::atomic<std::uint64_t> worker_tid{0};
    std::atomic<bool> run{true};
    std::thread w(spark::selftest::worker, std::ref(worker_tid), std::ref(run));
    while (worker_tid.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }

    const auto verify = [](const char *name, bool result) {
        if (!result) {
            std::fprintf(stderr, "selftest failed: %s\n", name);
        }
        return result;
    };

    if (!verify("argument parsing", spark::selftest::verifyArgumentParsing()) ||
        !verify("continuous history", spark::SamplerTestAccess::verifyContinuousHistory()) ||
        !verify("thread selector", spark::selftest::verifyThreadSelectorSemantics()) ||
        !verify("tick monitor", spark::selftest::verifyTickMonitor()) ||
        !verify("statistics service", spark::selftest::verifyStatisticsService()) ||
        !verify("system resource stats", spark::selftest::verifySystemResourceStats()) ||
        !verify("world gauge statistics", spark::selftest::verifyWorldGaugeStatistics()) ||
        !verify("absent world gauges", spark::selftest::verifyWorldGaugeAbsentWhenNotRecorded()) ||
        !verify("thread discovery", spark::selftest::verifyThreadDiscovery()) ||
        !verify("multi-thread serialization", spark::selftest::verifyMultiThreadSerialization()) ||
        !verify("health server configurations", spark::selftest::verifyHealthServerConfigurations()) ||
        !verify("live profiler windows", spark::selftest::verifyLiveProfilerWindowStatistics(worker_tid.load())) ||
        !verify("live export stop/cancel", spark::selftest::verifyLiveExportStopCancel(worker_tid.load())) ||
        !verify("live export timeout", spark::selftest::verifyLiveExportTimeout(worker_tid.load())) ||
        !verify("viewer shutdown", spark::selftest::verifyViewerShutdownDuringLiveExport(worker_tid.load())) ||
        !verify("viewer disconnect", spark::selftest::verifyViewerDisconnectKeepsProfilerRunning(worker_tid.load())) ||
#ifdef __linux__
        !spark::selftest::verifyAllocationViewerLifecycle(worker_tid.load()) ||
#endif
        !spark::selftest::verifyWorkerExceptionBoundaries(worker_tid.load()) ||
        !spark::selftest::verifyAsyncNetworkCommands(worker_tid.load()) ||
        !spark::selftest::verifyBackgroundCommandValidation(worker_tid.load()) ||
        !spark::selftest::verifyRecoveryWriterLifetime(worker_tid.load()) || !spark::selftest::verifyUploadFailure() ||
        !spark::selftest::verifyCaptureLifecycle() ||
#ifdef __linux__
        !spark::selftest::verifyDelayedSignalLifecycle() ||
        !spark::selftest::verifyActiveCaptureTeardown(worker_tid.load()) ||
#endif
#ifdef _WIN32
        !spark::selftest::verifyWindowsThreadActivityDetection() ||
#endif
        !spark::selftest::verifyAllThreadSampling() ||
        !spark::selftest::verifySelectedThreadSampling(worker_tid.load()) || !spark::selftest::verifyExecutableHash() ||
        !spark::selftest::verifyByteSampling() || !spark::selftest::verifyStopResponsiveness() ||
        !spark::selftest::verifySessionIsolation(worker_tid.load()) ||
        !spark::selftest::verifyTickFiltering(worker_tid.load())
#ifdef __linux__
        || !spark::selftest::verifyLinuxImportHooks() || !spark::selftest::verifyAllocationLifecycle() ||
        !spark::selftest::verifyAllocationThreadSelection() ||
        !spark::selftest::verifyProcessWideAllocationSampling() ||
        !spark::selftest::verifyAllocationContentionPolicy() || !spark::selftest::verifyAllocationResourcePressure()
#endif
    ) {
        run.store(false);
        w.join();
        return 1;
    }

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.interval_ms = 4;
    options.ignore_sleeping = true;

    std::string error;
    if (!profiler.start(options, worker_tid.load(), error)) {
        std::fprintf(stderr, "profiler start failed: %s\n", error.c_str());
        run.store(false);
        w.join();
        return 1;
    }

    // Drive ~20 "ticks" per second so windows/bucketing exercise like a real server.
    for (int i = 0; i < seconds * 20; ++i) {
        std::this_thread::sleep_for(50ms);
        profiler.onTick(30.0);
    }

    spark::ExportContext ctx;
    ctx.endstone_version = "0.11.5";
    ctx.minecraft_version = "1.26.33";
    std::string executable_hash_error;
    ctx.bds_executable_sha256 = spark::currentExecutableSha256(executable_hash_error);
    std::string bytes = profiler.stop(ctx);

    if (bytes.find("BDS executable SHA-256") == std::string::npos ||
        bytes.find(ctx.bds_executable_sha256) == std::string::npos) {
        std::fprintf(stderr, "executable hash: profile metadata is missing\n");
        run.store(false);
        w.join();
        return 1;
    }

    run.store(false);
    w.join();

    std::ofstream("profile.pb", std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::string gz = spark::gzipCompress(bytes);
    std::ofstream("profile.sparkprofile", std::ios::binary).write(gz.data(), static_cast<std::streamsize>(gz.size()));

    const std::filesystem::path profile_root =
        std::filesystem::temp_directory_path() /
        ("spark-profile-selftest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path profile_directory = spark::profileStorageDirectory(profile_root);
    spark::ProfileFileResult saved = spark::saveProfileToDirectory(profile_directory, gz, 42);
    if (!saved.ok) {
        std::fprintf(stderr, "profile file: atomic save failed: %s\n", saved.error.c_str());
        return 1;
    }
    if (saved.path.parent_path() != profile_directory) {
        std::fprintf(stderr, "profile file: local profile used the wrong directory\n");
        std::error_code cleanup_error;
        std::filesystem::remove_all(profile_root, cleanup_error);
        return 1;
    }
    std::ifstream saved_stream(saved.path, std::ios::binary);
    std::string round_trip((std::istreambuf_iterator<char>(saved_stream)), std::istreambuf_iterator<char>());
    saved_stream.close();
    std::error_code cleanup_error;
    std::filesystem::remove_all(profile_root, cleanup_error);
    if (round_trip != gz || cleanup_error) {
        std::fprintf(stderr, "profile file: saved gzip payload did not round-trip cleanly\n");
        return 1;
    }

    std::printf("samples=%llu proto=%zuB gzip=%zuB\n", static_cast<unsigned long long>(profiler.sampleCount()),
                bytes.size(), gz.size());
    std::printf("wrote profile.pb, profile.sparkprofile\n");

    if (upload) {
        auto result = spark::uploadToBytebin(gz, spark::kBytebinUrl, spark::kSamplerContentType,
                                             std::string("endstone-spark/") + spark::kVersion);
        if (result.ok) {
            std::printf("%s%s\n", spark::kViewerUrl, result.key.c_str());
        }
        else {
            std::printf("upload failed: %s\n", result.error.c_str());
        }
    }
    return 0;
}
