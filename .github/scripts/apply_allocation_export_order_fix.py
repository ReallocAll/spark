#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}: {old[:80]!r}")
    file.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/core/profiler/profiler.h",
    """    bool stopSampling(std::string &error);\n    void stopSampling();  // compatibility helper that discards the error\n""",
    """    bool stopSampling(std::string &error);\n    // Restarts persistent allocation-rate counting after a stopped allocation\n    // profile has been serialized. Safe to call when counting is disabled or\n    // already active.\n    bool resumePersistentAllocationCounting(std::string &error);\n    void stopSampling();  // compatibility helper that discards the error\n""",
)

old_stop = r'''bool Profiler::stopSampling(std::string &error)
{
    sampling_stop_requested_.store(true, std::memory_order_release);
    if (stop_requested_hook_) {
        stop_requested_hook_();
    }
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    if (!running_.load()) {
        return true;
    }
    const std::int64_t requested_end_time_ms = nowMs();
    if (mode_ == ProfileMode::Allocation) {
        if (!allocation_sampler_.stop(error)) {
            if (!allocation_sampler_.running()) {
                const bool resume_persistent = persistent_allocation_counting_enabled_.load(std::memory_order_acquire);
                if (resume_persistent) {
                    accumulatePersistentAllocationBytes();
                }
                running_.store(false);
                if (resume_persistent) {
                    std::string resume_error;
                    if (!startPersistentAllocationCounting(resume_error)) {
                        persistent_allocation_counting_enabled_.store(false, std::memory_order_release);
                    }
                }
            }
            return false;
        }
        if (persistent_allocation_counting_enabled_.load(std::memory_order_acquire)) {
            accumulatePersistentAllocationBytes();
        }
        stopRecoveryWriter();
    }
    else {
        if (!sampler_.stop()) {
            error = sampler_.lastError();
            return false;
        }
        stopRecoveryWriter();
        if (sampler_.failure(error)) {
            running_.store(false);
            end_time_ms_ = requested_end_time_ms;
            return false;
        }
    }
    running_.store(false);
    end_time_ms_ = requested_end_time_ms;
    if (mode_ == ProfileMode::Allocation && persistent_allocation_counting_enabled_.load(std::memory_order_acquire)) {
        std::string resume_error;
        if (!startPersistentAllocationCounting(resume_error)) {
            persistent_allocation_counting_enabled_.store(false, std::memory_order_release);
        }
    }
    return true;
}
'''

new_stop = r'''bool Profiler::stopSampling(std::string &error)
{
    sampling_stop_requested_.store(true, std::memory_order_release);
    if (stop_requested_hook_) {
        stop_requested_hook_();
    }
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    if (!running_.load()) {
        return true;
    }
    const std::int64_t requested_end_time_ms = nowMs();
    if (mode_ == ProfileMode::Allocation) {
        if (!allocation_sampler_.stop(error)) {
            if (!allocation_sampler_.running()) {
                if (persistent_allocation_counting_enabled_.load(std::memory_order_acquire)) {
                    accumulatePersistentAllocationBytes();
                }
                running_.store(false);
                end_time_ms_ = requested_end_time_ms;
            }
            return false;
        }
        if (persistent_allocation_counting_enabled_.load(std::memory_order_acquire)) {
            accumulatePersistentAllocationBytes();
        }
        stopRecoveryWriter();
    }
    else {
        if (!sampler_.stop()) {
            error = sampler_.lastError();
            return false;
        }
        stopRecoveryWriter();
        if (sampler_.failure(error)) {
            running_.store(false);
            end_time_ms_ = requested_end_time_ms;
            return false;
        }
    }
    running_.store(false);
    end_time_ms_ = requested_end_time_ms;
    // Do not restart persistent count-only here. startPersistentAllocationCounting()
    // resets the allocation sampler session, including its completed call tree.
    // Normal profile export is a two-phase stopSampling() -> exportData() flow, so
    // the exporter resumes persistent counting immediately after serialization.
    return true;
}

bool Profiler::resumePersistentAllocationCounting(std::string &error)
{
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    if (!persistent_allocation_counting_enabled_.load(std::memory_order_acquire) ||
        persistent_allocation_counting_active_.load(std::memory_order_acquire)) {
        return true;
    }
    // A full allocation profile itself accounts allocation bytes. Never start a
    // second allocation sampler while that profile is still active.
    if (running_.load(std::memory_order_acquire) && mode_ == ProfileMode::Allocation) {
        return true;
    }
    if (!startPersistentAllocationCounting(error)) {
        persistent_allocation_counting_enabled_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}
'''
replace_once("src/core/profiler/profiler.cpp", old_stop, new_stop)

replace_once(
    "src/core/profiler/profiler.cpp",
    r'''    if (!stopSampling(error)) {
        return {};
    }
    return exportData(ctx);
}''',
    r'''    if (!stopSampling(error)) {
        std::string ignored;
        resumePersistentAllocationCounting(ignored);
        return {};
    }
    try {
        std::string data = exportData(ctx);
        std::string ignored;
        resumePersistentAllocationCounting(ignored);
        return data;
    }
    catch (...) {
        std::string ignored;
        resumePersistentAllocationCounting(ignored);
        throw;
    }
}''',
)

replace_once(
    "src/core/profiler/profiler.cpp",
    r'''    if (stopSampling(stop_error)) {
        error.clear();
        discardRecoveryJournal();
        return true;
    }

    // A backend failure invalidates the data; if stopSampling completed native cleanup,
    // cancel is still successful. Persistent count-only may already have restarted
    // and reset the allocation backend's per-session failure state at this point.
    if (!running_.load()) {
        error.clear();
        discardRecoveryJournal();
        return true;
    }''',
    r'''    if (stopSampling(stop_error)) {
        std::string resume_error;
        resumePersistentAllocationCounting(resume_error);
        error.clear();
        discardRecoveryJournal();
        return true;
    }

    // A backend failure invalidates the data; if stopSampling completed native cleanup,
    // cancel is still successful. Resume persistent count-only because there will be no
    // export phase to do it for us.
    if (!running_.load()) {
        std::string resume_error;
        resumePersistentAllocationCounting(resume_error);
        error.clear();
        discardRecoveryJournal();
        return true;
    }''',
)

replace_once(
    "src/application/profiler/profile_exporter.h",
    "Result exportProfile(const Profiler &profiler, const ExportContext &ctx, bool save_to_file);",
    "Result exportProfile(Profiler &profiler, const ExportContext &ctx, bool save_to_file);",
)
replace_once(
    "src/application/profiler/profile_exporter.cpp",
    "ProfileExporter::Result ProfileExporter::exportProfile(const Profiler &profiler, const ExportContext &ctx,\n                                                       bool save_to_file)",
    "ProfileExporter::Result ProfileExporter::exportProfile(Profiler &profiler, const ExportContext &ctx,\n                                                       bool save_to_file)",
)
replace_once(
    "src/application/profiler/profile_exporter.cpp",
    r'''        std::string body = profiler.exportData(ctx);
        std::string compressed = gzipCompress(body);''',
    r'''        std::string body = profiler.exportData(ctx);
        // Serialization has copied the completed allocation tree. Persistent
        // count-only may now safely reset/reuse the native allocation sampler
        // while gzip and network/file I/O continue in this export worker.
        std::string resume_error;
        profiler.resumePersistentAllocationCounting(resume_error);
        std::string compressed = gzipCompress(body);''',
)
replace_once(
    "src/application/profiler/profile_exporter.cpp",
    r'''    catch (const std::exception &e) {
        result.message = std::string("Export failed: ") + e.what();
    }
    catch (...) {
        result.message = "Export failed with an unknown error.";
    }''',
    r'''    catch (const std::exception &e) {
        std::string ignored;
        profiler.resumePersistentAllocationCounting(ignored);
        result.message = std::string("Export failed: ") + e.what();
    }
    catch (...) {
        std::string ignored;
        profiler.resumePersistentAllocationCounting(ignored);
        result.message = "Export failed with an unknown error.";
    }''',
)

replace_once(
    "src/application/profiler/profiler_service.cpp",
    "if (!profiler_.stopSampling(stop_error)) {",
    "if (!profiler_.stopSampling(stop_error)) {",
)
# The stop call itself stays identical; the changed Profiler semantics preserve
# the completed allocation tree until ProfileExporter serializes it. Ensure all
# pre-export early-return paths resume persistent count-only.
replace_once(
    "src/application/profiler/profiler_service.cpp",
    r'''        if (stopped) {
            session_type_ = SessionType::None;
            restore_background();
        }''',
    r'''        if (stopped) {
            session_type_ = SessionType::None;
            std::string resume_error;
            profiler_.resumePersistentAllocationCounting(resume_error);
            restore_background();
        }''',
)
replace_once(
    "src/application/profiler/profiler_service.cpp",
    r'''    catch (const std::exception &error) {
        restore_background();
        try {''',
    r'''    catch (const std::exception &error) {
        std::string resume_error;
        profiler_.resumePersistentAllocationCounting(resume_error);
        restore_background();
        try {''',
)
replace_once(
    "src/application/profiler/profiler_service.cpp",
    r'''    catch (...) {
        restore_background();
        notify_best_effort(sender_name, "Failed to prepare the profile export.");
        return;
    }''',
    r'''    catch (...) {
        std::string resume_error;
        profiler_.resumePersistentAllocationCounting(resume_error);
        restore_background();
        notify_best_effort(sender_name, "Failed to prepare the profile export.");
        return;
    }''',
)
replace_once(
    "src/application/profiler/profiler_service.cpp",
    r'''    catch (...) {
        exporting_.store(false);
        restore_background();
        notify_best_effort(sender_name, "Failed to start the profile export worker.");
    }''',
    r'''    catch (...) {
        exporting_.store(false);
        std::string resume_error;
        profiler_.resumePersistentAllocationCounting(resume_error);
        restore_background();
        notify_best_effort(sender_name, "Failed to start the profile export worker.");
    }''',
)

replace_once(
    "tests/core/profiler/profiler_export_test.cpp",
    r'''    static std::uint64_t allocationContentionDropped(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.contentionDropped();
    }
};''',
    r'''    static std::uint64_t allocationContentionDropped(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.contentionDropped();
    }

    static bool persistentAllocationCountingActive(const Profiler &profiler)
    {
        return profiler.persistent_allocation_counting_active_.load(std::memory_order_acquire);
    }
};''',
)

insert_before = r'''bool verifyRetainedAllocationProfile()
{'''
new_test = r'''bool verifyPersistentAllocationExportOrdering()
{
    using namespace std::chrono_literals;

    spark::Profiler profiler;
    const std::uint64_t server_tid = spark::currentNativeThreadId();
    std::string error;
    if (!profiler.setPersistentAllocationCountingEnabled(true, server_tid, error) ||
        !spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: count-only setup failed: %s\n", error.c_str());
        return false;
    }

    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 1;
    if (!profiler.start(options, server_tid, error)) {
        std::fprintf(stderr, "persistent export: allocation start failed: %s\n", error.c_str());
        return false;
    }
    if (spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: count-only remained active during full profile\n");
        profiler.cancel(error);
        return false;
    }
    if (!exerciseNativeAllocations() ||
        !waitForCondition([&] { return profiler.sampleCount() != 0; }, 2s)) {
        std::fprintf(stderr, "persistent export: no allocation samples were collected\n");
        profiler.cancel(error);
        return false;
    }
    const std::uint64_t samples_before_stop = profiler.sampleCount();
    const std::uint64_t bytes_before_stop = profiler.sampledAllocationBytes();

    if (!profiler.stopSampling(error)) {
        std::fprintf(stderr, "persistent export: stop failed: %s\n", error.c_str());
        return false;
    }
    if (spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) ||
        profiler.sampleCount() != samples_before_stop || profiler.sampledAllocationBytes() != bytes_before_stop) {
        std::fprintf(stderr,
                     "persistent export: completed profile was reset before export "
                     "(samples=%llu/%llu bytes=%llu/%llu active=%d)\n",
                     static_cast<unsigned long long>(profiler.sampleCount()),
                     static_cast<unsigned long long>(samples_before_stop),
                     static_cast<unsigned long long>(profiler.sampledAllocationBytes()),
                     static_cast<unsigned long long>(bytes_before_stop),
                     static_cast<int>(spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)));
        return false;
    }

    const std::string profile = profiler.exportData({});
    if (profile.empty() || profiler.sampleCount() != samples_before_stop) {
        std::fprintf(stderr, "persistent export: serialization did not preserve completed samples\n");
        return false;
    }
    if (!profiler.resumePersistentAllocationCounting(error) ||
        !spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: count-only resume failed: %s\n", error.c_str());
        return false;
    }
    return profiler.shutdown(error);
}

bool verifyRetainedAllocationProfile()
{'''
replace_once("tests/core/profiler/profiler_export_test.cpp", insert_before, new_test)
replace_once(
    "tests/core/profiler/profiler_export_test.cpp",
    r'''#ifdef __linux__
    if (!verifyRetainedAllocationProfile() || !verifyAllocationLiveExport() || !verifyRetainedAllocationLiveExport()) {
        return 1;
    }
#endif''',
    r'''#if defined(_WIN32) || defined(__linux__)
    if (!verifyPersistentAllocationExportOrdering()) {
        return 1;
    }
#endif
#ifdef __linux__
    if (!verifyRetainedAllocationProfile() || !verifyAllocationLiveExport() || !verifyRetainedAllocationLiveExport()) {
        return 1;
    }
#endif''',
)

print("allocation export ordering patch applied")
