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

bool verifySystemResourceStats()
{
    const spark::ProcessStats process = spark::gatherProcessStats();
    if (!process.rss_present || process.rss_bytes <= 0 || !process.virtual_present ||
        process.virtual_bytes < process.rss_bytes || !process.threads_present || process.threads < 1) {
        std::fprintf(stderr,
                     "system resources: process RSS/virtual memory/thread query failed "
                     "(rss=%lld virtual=%lld threads=%d)\n",
                     static_cast<long long>(process.rss_bytes), static_cast<long long>(process.virtual_bytes),
                     process.threads);
        return false;
    }

    const spark::SystemStats system = spark::gatherSystemStats(".");
    if (!system.present || !system.cpu_present || system.cpu_threads < 1 || !system.memory_present ||
        system.mem_total <= 0 || system.mem_used < 0 || system.mem_used > system.mem_total || !system.swap_present ||
        system.swap_total < 0 || system.swap_used < 0 || system.swap_used > system.swap_total || !system.disk_present ||
        system.disk_total <= 0 || system.disk_used < 0 || system.disk_used > system.disk_total || !system.os_present ||
        system.os_name.empty() || system.os_arch.empty()) {
        std::fprintf(stderr, "system resources: host availability/value validation "
                             "failed\n");
        return false;
    }
    return true;
}

bool verifyStatisticsService()
{
    auto close = [](double actual, double expected) {
        return std::abs(actual - expected) < 0.000001;
    };
    auto initial_cpu = [] {
        spark::CpuSnapshot snapshot;
        snapshot.valid = true;
        snapshot.process_ticks_per_second = 100.0;
        snapshot.cpu_threads = 2;
        snapshot.wall_ms = 0;
        return snapshot;
    };

    auto tps_service = std::make_unique<spark::StatisticsService>();
    tps_service->startAt(0, 1'000'000, initial_cpu());
    for (int second = 0; second < 900; ++second) {
        int rate = 5;
        if (second < 600) {
            rate = 20;
        }
        else if (second < 840) {
            rate = 18;
        }
        else if (second < 890) {
            rate = 15;
        }
        else if (second < 895) {
            rate = 10;
        }
        for (int tick = 1; tick <= rate; ++tick) {
            const std::int64_t timestamp =
                static_cast<std::int64_t>(second) * 1000 + static_cast<std::int64_t>(tick) * 1000 / rate;
            tps_service->recordTickAt(2.0, timestamp);
        }
    }
    const spark::StatisticsSnapshot tps = tps_service->snapshotAt(900'000);
    if (!close(tps.tps.last_5s.value, 5.0) || !close(tps.tps.last_10s.value, 7.5) ||
        !close(tps.tps.last_1m.value, 13.75) || !close(tps.tps.last_5m.value, 17.15) ||
        !close(tps.tps.last_15m.value, 19.05) || tps.tps.last_5s.samples != 25 || tps.history_span_ms != 900'000) {
        std::fprintf(stderr, "statistics service: TPS windows were not independently "
                             "time-weighted\n");
        return false;
    }

    auto mspt_service = std::make_unique<spark::StatisticsService>();
    mspt_service->startAt(0, 2'000'000, initial_cpu());
    for (int tick = 1; tick <= 1000; ++tick) {
        mspt_service->recordTickAt(static_cast<double>((tick - 1) % 100 + 1), static_cast<std::int64_t>(tick) * 10);
    }
    const spark::StatisticsSnapshot mspt = mspt_service->snapshotAt(10'000);
    const spark::DistributionValues &distribution = mspt.mspt.last_10s;
    if (!distribution.present || distribution.samples != 1000 || !close(distribution.mean, 50.5) ||
        !close(distribution.min, 1.0) || !close(distribution.median, 50.5) || !close(distribution.percentile95, 95.0) ||
        !close(distribution.max, 100.0) || mspt.mspt.last_1m.span_ms != 10'000) {
        std::fprintf(stderr, "statistics service: MSPT distribution or partial-window "
                             "span was incorrect\n");
        return false;
    }

    auto cpu_service = std::make_unique<spark::StatisticsService>();
    spark::CpuSnapshot cpu = initial_cpu();
    cpu_service->startAt(0, 3'000'000, cpu);
    for (int second = 1; second <= 900; ++second) {
        std::uint64_t process_delta = 80;
        if (second <= 840) {
            process_delta = 20;
        }
        else if (second <= 890) {
            process_delta = 40;
        }
        const std::uint64_t busy_delta = process_delta;
        cpu.process_ticks += process_delta;
        cpu.system_busy += busy_delta;
        cpu.system_total += 100;
        cpu.wall_ms = static_cast<std::int64_t>(second) * 1000;
        cpu_service->recordCpuSnapshot(cpu);
    }
    const spark::StatisticsSnapshot cpu_stats = cpu_service->snapshotAt(900'000);
    if (!close(cpu_stats.cpu.process_last_10s.value, 0.4) || !close(cpu_stats.cpu.process_last_1m.value, 14.0 / 60.0) ||
        !close(cpu_stats.cpu.process_last_15m.value, 98.0 / 900.0) ||
        !close(cpu_stats.cpu.system_last_10s.value, 0.8) || !close(cpu_stats.cpu.system_last_1m.value, 28.0 / 60.0) ||
        !close(cpu_stats.cpu.system_last_15m.value, 196.0 / 900.0)) {
        std::fprintf(stderr, "statistics service: CPU windows were not independently "
                             "time-weighted\n");
        return false;
    }

    auto window_service = std::make_unique<spark::StatisticsService>();
    spark::CpuSnapshot window_cpu = initial_cpu();
    const std::int32_t window_adjustment = spark::profiling_window::windowAdjustmentMs();
    const std::int64_t window_profile_start = spark::profiling_window::windowStartTime(
        spark::profiling_window::timeToWindow(4'000'000, window_adjustment) + 1, window_adjustment);
    const std::int64_t window_profile_end = window_profile_start + 2 * spark::profiling_window::kSizeMs;
    window_service->startAt(0, window_profile_start, window_cpu);
    window_service->recordPlayerCountAt(2, 0);
    window_service->recordTickAt(1.0, 100);
    window_service->recordTickAt(9.0, 600);
    window_cpu.process_ticks += 1'200;
    window_cpu.system_busy += 1'200;
    window_cpu.system_total += 6'000;
    window_cpu.wall_ms = 60'000;
    window_service->recordCpuSnapshot(window_cpu);
    window_service->recordPlayerCountAt(3, 60'000);
    window_service->recordTickAt(2.0, 60'100);
    window_service->recordTickAt(8.0, 60'600);
    window_cpu.process_ticks += 2'400;
    window_cpu.system_busy += 2'400;
    window_cpu.system_total += 6'000;
    window_cpu.wall_ms = 120'000;
    window_service->recordCpuSnapshot(window_cpu);
    const auto windows = window_service->profileWindows(window_profile_start, window_profile_end);
    const std::int32_t first_window_id = spark::profiling_window::timeToWindow(window_profile_start, window_adjustment);
    const std::int32_t second_window_id = spark::profiling_window::timeToWindow(
        window_profile_start + spark::profiling_window::kSizeMs, window_adjustment);
    auto first_window = windows.find(first_window_id);
    auto second_window = windows.find(second_window_id);
    const std::int32_t adjustment_ms = window_adjustment;
    const std::int64_t first_window_start = spark::profiling_window::windowStartTime(first_window_id, adjustment_ms);
    const std::int64_t first_window_end = spark::profiling_window::windowEndTime(first_window_id, adjustment_ms);
    if (windows.size() != 2 || first_window == windows.end() || second_window == windows.end() ||
        first_window->second.ticks != 2 || !close(first_window->second.tps, 2.0 / 60.0) ||
        !close(first_window->second.mspt_median, 5.0) || !close(first_window->second.mspt_max, 9.0) ||
        !close(first_window->second.cpu_process, 0.1) || !close(first_window->second.cpu_system, 0.2) ||
        first_window->second.players != 3 || first_window->second.start_time_ms != first_window_start ||
        first_window->second.end_time_ms != first_window_end ||
        first_window->second.duration_ms != spark::profiling_window::kSizeMs || second_window->second.players != 3 ||
        second_window->second.entities_present || second_window->second.chunks_present) {
        std::fprintf(stderr,
                     "statistics service: per-minute profile windows were "
                     "incorrect (count=%zu first ticks=%d tps=%.3f "
                     "median=%.3f max=%.3f process=%.3f system=%.3f "
                     "players=%d start=%lld end=%lld duration=%d; "
                     "second players=%d)\n",
                     windows.size(), first_window == windows.end() ? -1 : first_window->second.ticks,
                     first_window == windows.end() ? -1.0 : first_window->second.tps,
                     first_window == windows.end() ? -1.0 : first_window->second.mspt_median,
                     first_window == windows.end() ? -1.0 : first_window->second.mspt_max,
                     first_window == windows.end() ? -1.0 : first_window->second.cpu_process,
                     first_window == windows.end() ? -1.0 : first_window->second.cpu_system,
                     first_window == windows.end() ? -1 : first_window->second.players,
                     static_cast<long long>(first_window == windows.end() ? -1 : first_window->second.start_time_ms),
                     static_cast<long long>(first_window == windows.end() ? -1 : first_window->second.end_time_ms),
                     first_window == windows.end() ? -1 : first_window->second.duration_ms,
                     second_window == windows.end() ? -1 : second_window->second.players);
        return false;
    }

    auto background_service = std::make_unique<spark::StatisticsService>();
    const std::int64_t background_profile_start = spark::profiling_window::windowStartTime(
        spark::profiling_window::timeToWindow(7'000'000, window_adjustment) + 1, window_adjustment);
    const std::int64_t background_profile_end = background_profile_start + 30 * spark::profiling_window::kSizeMs;
    background_service->startAt(0, background_profile_start, initial_cpu());
    for (int minute = 0; minute < 30; ++minute) {
        background_service->recordTickAt(5.0, static_cast<std::int64_t>(minute) * 60'000 + 100);
    }
    const auto background_windows =
        background_service->profileWindows(background_profile_start, background_profile_end);
    const std::int32_t background_first_window =
        spark::profiling_window::timeToWindow(background_profile_start, window_adjustment);
    const std::int32_t background_last_window =
        spark::profiling_window::timeToWindow(background_profile_end - 1, window_adjustment);
    if (background_windows.size() != 30 || !background_windows.contains(background_first_window) ||
        !background_windows.contains(background_last_window) ||
        background_windows.at(background_first_window).duration_ms != spark::profiling_window::kSizeMs ||
        background_windows.at(background_last_window).duration_ms != spark::profiling_window::kSizeMs) {
        std::fprintf(stderr, "statistics service: background Refine history did not retain 30 minute windows\n");
        return false;
    }

    const auto clipped_windows =
        background_service->profileWindows(background_profile_start - spark::profiling_window::kSizeMs / 2,
                                           background_profile_start + spark::profiling_window::kSizeMs);
    if (clipped_windows.size() != 1 || !clipped_windows.contains(background_first_window) ||
        clipped_windows.at(background_first_window).start_time_ms != background_profile_start ||
        clipped_windows.at(background_first_window).end_time_ms !=
            background_profile_start + spark::profiling_window::kSizeMs) {
        std::fprintf(stderr, "statistics service: profile window history was not clipped to service start\n");
        return false;
    }
    return true;
}

bool verifyWorldGaugeStatistics()
{
    spark::CpuSnapshot cpu;
    cpu.valid = true;
    cpu.process_ticks_per_second = 100.0;
    cpu.cpu_threads = 2;
    cpu.wall_ms = 0;

    auto svc = std::make_unique<spark::StatisticsService>();
    const std::int32_t window_adjustment = spark::profiling_window::windowAdjustmentMs();
    const std::int64_t profile_start = spark::profiling_window::windowStartTime(
        spark::profiling_window::timeToWindow(5'000'000, window_adjustment) + 1, window_adjustment);
    const std::int64_t profile_end = profile_start + 2 * spark::profiling_window::kSizeMs;
    svc->startAt(0, profile_start, cpu);
    svc->recordPlayerCountAt(1, 0);
    svc->recordWorldGaugesAt(10, 20, 0);
    svc->recordTickAt(5.0, 100);
    svc->recordTickAt(5.0, 600);
    cpu.process_ticks += 1'200;
    cpu.system_busy += 1'200;
    cpu.system_total += 6'000;
    cpu.wall_ms = 60'000;
    svc->recordCpuSnapshot(cpu);
    svc->recordPlayerCountAt(2, 60'000);
    svc->recordWorldGaugesAt(15, 25, 60'000);
    svc->recordTickAt(5.0, 60'100);
    svc->recordTickAt(5.0, 60'600);
    cpu.process_ticks += 2'400;
    cpu.system_busy += 2'400;
    cpu.system_total += 6'000;
    cpu.wall_ms = 120'000;
    svc->recordCpuSnapshot(cpu);
    svc->recordPlayerCountAt(3, 120'000);
    svc->recordWorldGaugesAt(20, 30, 120'000);

    const auto windows = svc->profileWindows(profile_start, profile_end);
    const std::int32_t first_window_id = spark::profiling_window::timeToWindow(profile_start, window_adjustment);
    const std::int32_t second_window_id =
        spark::profiling_window::timeToWindow(profile_start + spark::profiling_window::kSizeMs, window_adjustment);
    auto first = windows.find(first_window_id);
    auto second = windows.find(second_window_id);
    // The gauge loop picks the last sample within each window's end time,
    // matching the existing players behavior.
    if (windows.size() != 2 || first == windows.end() || second == windows.end() || !first->second.entities_present ||
        first->second.entities != 15 || !first->second.chunks_present || first->second.chunks != 25 ||
        !second->second.entities_present || second->second.entities != 20 || !second->second.chunks_present ||
        second->second.chunks != 30) {
        std::fprintf(stderr,
                     "world gauge statistics: entities/chunks not correct "
                     "(first ents=%d chunks=%d; second ents=%d chunks=%d)\n",
                     first == windows.end() ? -1 : first->second.entities,
                     first == windows.end() ? -1 : first->second.chunks,
                     second == windows.end() ? -1 : second->second.entities,
                     second == windows.end() ? -1 : second->second.chunks);
        return false;
    }
    return true;
}

bool verifyWorldGaugeAbsentWhenNotRecorded()
{
    spark::CpuSnapshot cpu;
    cpu.valid = true;
    cpu.process_ticks_per_second = 100.0;
    cpu.cpu_threads = 2;
    cpu.wall_ms = 0;

    auto svc = std::make_unique<spark::StatisticsService>();
    const std::int32_t window_adjustment = spark::profiling_window::windowAdjustmentMs();
    const std::int64_t profile_start = spark::profiling_window::windowStartTime(
        spark::profiling_window::timeToWindow(6'000'000, window_adjustment) + 1, window_adjustment);
    const std::int64_t profile_end = profile_start + spark::profiling_window::kSizeMs;
    svc->startAt(0, profile_start, cpu);
    svc->recordPlayerCountAt(1, 0);
    svc->recordTickAt(5.0, 100);
    svc->recordTickAt(5.0, 600);
    cpu.process_ticks += 20;
    cpu.system_busy += 20;
    cpu.system_total += 100;
    cpu.wall_ms = 60'000;
    svc->recordCpuSnapshot(cpu);

    const auto windows = svc->profileWindows(profile_start, profile_end);
    const std::int32_t first_window_id = spark::profiling_window::timeToWindow(profile_start, window_adjustment);
    auto first = windows.find(first_window_id);
    if (windows.size() != 1 || first == windows.end() || first->second.entities_present ||
        first->second.chunks_present) {
        std::fprintf(stderr,
                     "world gauge absent: entities_present=%d chunks_present=%d "
                     "(expected both false)\n",
                     first == windows.end() ? -1 : static_cast<int>(first->second.entities_present),
                     first == windows.end() ? -1 : static_cast<int>(first->second.chunks_present));
        return false;
    }
    return true;
}

bool verifyExecutableHash()
{
    if (spark::sha256Hex("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
        spark::sha256Hex("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::fprintf(stderr, "executable hash: SHA-256 vector mismatch\n");
        return false;
    }

    std::string error;
    const std::string first = spark::currentExecutableSha256(error);
    const std::string second = spark::currentExecutableSha256(error);
    if (first.size() != 64 || first != second || !std::ranges::all_of(first, [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) {
        std::fprintf(stderr, "executable hash: current executable hashing failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

}  // namespace spark::selftest
