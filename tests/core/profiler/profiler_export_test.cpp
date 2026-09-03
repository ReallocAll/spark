#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

#include "core/profiler/profiler.h"
#include "native/sampler/thread_info.h"
#include "proto/proto_reader.h"

namespace spark {

struct ProfilerTestAccess {
    static void setMode(Profiler &profiler, ProfileMode mode) { profiler.mode_ = mode; }

    static bool allocationSnapshot(Profiler &profiler, AllocationSnapshot &snapshot, std::string &error)
    {
        return profiler.allocation_sampler_.snapshot(snapshot, error);
    }

    static bool allocationSamplerRunning(const Profiler &profiler) { return profiler.allocation_sampler_.running(); }

    static bool allocationHooksInstalled(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.hooksInstalled();
    }

    static std::uint64_t allocationLifecycleDropped(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.lifecycleDropped();
    }

    static std::uint64_t allocationContentionDropped(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.contentionDropped();
    }

    static std::uint64_t allocationTerminalSamples(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.terminalInFlightTickSamplesDiscarded();
    }

    static std::uint64_t allocationDroppedSamples(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.droppedSamples();
    }

    static std::uint64_t allocationPendingSamples(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.pendingSampleDrops();
    }

    static bool allocationDataIncomplete(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.dataIncomplete();
    }

    static void seedExecutionTerminalSamples(Profiler &profiler, std::size_t count)
    {
        profiler.mode_ = ProfileMode::Execution;
        profiler.sampler_.resetSession();
        profiler.sampler_.config_.only_ticks_over_ms = 10;
        for (std::size_t i = 0; i < count; ++i) {
            Sample sample;
            sample.thread_id = 1;
            sample.thread_name = "thread";
            sample.window = 1;
            sample.tick_id = 7;
            sample.frames.push_back(FrameKey{.module = 1,
                                             .rva = static_cast<std::uint64_t>(i + 1),
                                             .raw_address = static_cast<std::uint64_t>(i + 1)});
            profiler.sampler_.buckets_[sample.tick_id].push_back(sample);
            ++profiler.sampler_.pending_sample_count_;
        }
        profiler.sampler_.finishPending(7);
    }

    static bool persistentAllocationCountingActive(const Profiler &profiler)
    {
        return profiler.persistent_allocation_counting_active_.load(std::memory_order_acquire);
    }

    static void requestPersistentAllocationBackendStop(Profiler &profiler)
    {
        profiler.allocation_sampler_.requestStop();
    }
};

}  // namespace spark

namespace {

template <typename Predicate, typename Rep, typename Period>
bool waitForCondition(Predicate pred, std::chrono::duration<Rep, Period> timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

bool packedDoubleHasPositive(std::string_view bytes)
{
    if (bytes.empty() || bytes.size() % sizeof(double) != 0) {
        return false;
    }
    for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(double)) {
        double value = 0.0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        if (value > 0.0) {
            return true;
        }
    }
    return false;
}

bool findExtraMetadataValue(std::string_view profile, std::string_view key, std::string &value)
{
    spark::ProtoReader data(profile);
    int field = 0;
    int wire_type = 0;
    while (data.nextField(field, wire_type)) {
        if (field != 1 || wire_type != 2) {
            data.skip(wire_type);
            continue;
        }

        spark::ProtoReader metadata = data.readMessage();
        int metadata_field = 0;
        int metadata_wire_type = 0;
        while (metadata.nextField(metadata_field, metadata_wire_type)) {
            if (metadata_field != 14 || metadata_wire_type != 2) {
                metadata.skip(metadata_wire_type);
                continue;
            }
            spark::ProtoReader entry = metadata.readMessage();
            std::string_view entry_key;
            std::string_view entry_value;
            int entry_field = 0;
            int entry_wire_type = 0;
            while (entry.nextField(entry_field, entry_wire_type)) {
                if (entry_field == 1 && entry_wire_type == 2) {
                    entry_key = entry.readString();
                }
                else if (entry_field == 2 && entry_wire_type == 2) {
                    entry_value = entry.readString();
                }
                else {
                    entry.skip(entry_wire_type);
                }
            }
            if (!entry.valid()) {
                return false;
            }
            if (entry_key == key) {
                value.assign(entry_value);
                return metadata.valid() && data.valid();
            }
        }
        if (!metadata.valid()) {
            return false;
        }
    }
    return false;
}

bool metadataUnsigned(std::string_view profile, std::string_view key, std::uint64_t expected)
{
    std::string value;
    if (!findExtraMetadataValue(profile, key, value)) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size() && parsed == expected;
}

bool metadataBoolean(std::string_view profile, std::string_view key, bool expected)
{
    std::string value;
    return findExtraMetadataValue(profile, key, value) && value == (expected ? "true" : "false");
}

bool verifyTerminalMetadataExport()
{
    spark::Profiler execution;
    const std::string execution_profile = execution.exportData({});
    if (execution_profile.empty() ||
        !metadataUnsigned(execution_profile, "Execution terminal in-flight tick samples discarded", 0) ||
        !metadataUnsigned(execution_profile, "Execution samples dropped", 0) ||
        !metadataUnsigned(execution_profile, "Execution pending samples dropped", 0) ||
        !metadataBoolean(execution_profile, "Execution data incomplete", false)) {
        std::fprintf(stderr, "terminal metadata: execution values were not serialized correctly\n");
        return false;
    }

    spark::Profiler allocation;
    spark::ProfilerTestAccess::setMode(allocation, spark::ProfileMode::Allocation);
    const std::string allocation_profile = allocation.exportData({});
    if (allocation_profile.empty() ||
        !metadataUnsigned(allocation_profile, "Allocation terminal in-flight tick samples discarded", 0) ||
        !metadataUnsigned(allocation_profile, "Allocation pending final drops", 0) ||
        !metadataUnsigned(allocation_profile, "Allocation samples dropped", 0) ||
        !metadataUnsigned(allocation_profile, "Allocation pending samples dropped", 0) ||
        !metadataBoolean(allocation_profile, "Allocation data incomplete", false)) {
        std::fprintf(stderr, "terminal metadata: allocation values were not serialized correctly\n");
        return false;
    }
    return true;
}

#if defined(_WIN32) || defined(__linux__)
bool verifyTerminalMetadataExportWithSamples()
{
    spark::Profiler execution;
    spark::ProfilerTestAccess::seedExecutionTerminalSamples(execution, 4);
    const std::string execution_profile = execution.exportData({});
    if (!metadataUnsigned(execution_profile, "Execution terminal in-flight tick samples discarded", 4) ||
        !metadataUnsigned(execution_profile, "Execution samples dropped", 0) ||
        !metadataUnsigned(execution_profile, "Execution pending samples dropped", 0) ||
        !metadataBoolean(execution_profile, "Execution data incomplete", false)) {
        std::fprintf(stderr, "terminal metadata: execution nonzero values were not serialized correctly\n");
        return false;
    }

    spark::Profiler allocation;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.only_ticks_over_ms = 10;
    options.allocation_interval_bytes = 1;
    std::string error;
    if (!allocation.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "terminal metadata: allocation start failed: %s\n", error.c_str());
        return false;
    }
    std::vector<void *> retained;
    retained.reserve(1024);
    for (int i = 0; i < 1024; ++i) {
        void *pointer = std::malloc(256);
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
            retained.push_back(pointer);
        }
    }
    if (retained.empty() || !allocation.stopSampling(error)) {
        std::fprintf(stderr, "terminal metadata: allocation stop failed: %s\n", error.c_str());
        for (void *pointer : retained) {
            std::free(pointer);
        }
        allocation.shutdown(error);
        return false;
    }
    const std::uint64_t terminal = spark::ProfilerTestAccess::allocationTerminalSamples(allocation);
    const std::uint64_t dropped = spark::ProfilerTestAccess::allocationDroppedSamples(allocation);
    const std::uint64_t pending = spark::ProfilerTestAccess::allocationPendingSamples(allocation);
    const bool incomplete = spark::ProfilerTestAccess::allocationDataIncomplete(allocation);
    const std::string allocation_profile = allocation.exportData({});
    const bool valid =
        terminal != 0 &&
        metadataUnsigned(allocation_profile, "Allocation terminal in-flight tick samples discarded", terminal) &&
        metadataUnsigned(allocation_profile, "Allocation pending final drops", terminal) &&
        metadataUnsigned(allocation_profile, "Allocation samples dropped", dropped) &&
        metadataUnsigned(allocation_profile, "Allocation pending samples dropped", pending) &&
        metadataBoolean(allocation_profile, "Allocation data incomplete", incomplete);
    for (void *pointer : retained) {
        std::free(pointer);
    }
    if (!valid) {
        std::fprintf(stderr,
                     "terminal metadata: allocation nonzero values were not serialized correctly "
                     "(terminal=%llu dropped=%llu pending=%llu incomplete=%d)\n",
                     static_cast<unsigned long long>(terminal), static_cast<unsigned long long>(dropped),
                     static_cast<unsigned long long>(pending), static_cast<int>(incomplete));
        allocation.shutdown(error);
        return false;
    }
    return allocation.shutdown(error);
}
#endif

bool serializedAllocationTreeNonEmpty(std::string_view profile)
{
    bool allocation_mode = false;
    bool non_empty_thread_tree = false;
    spark::ProtoReader data(profile);
    int field = 0;
    int wire_type = 0;
    while (data.nextField(field, wire_type)) {
        if (field == 1 && wire_type == 2) {
            spark::ProtoReader metadata = data.readMessage();
            int metadata_field = 0;
            int metadata_wire = 0;
            while (metadata.nextField(metadata_field, metadata_wire)) {
                if (metadata_field == 15 && metadata_wire == 0) {
                    allocation_mode = metadata.readVarint() == 1;
                }
                else {
                    metadata.skip(metadata_wire);
                }
            }
            continue;
        }
        if (field == 2 && wire_type == 2) {
            spark::ProtoReader thread = data.readMessage();
            bool has_node = false;
            bool has_positive_root_weight = false;
            bool has_root_refs = false;
            int thread_field = 0;
            int thread_wire = 0;
            while (thread.nextField(thread_field, thread_wire)) {
                if (thread_field == 3 && thread_wire == 2) {
                    has_node = !thread.readString().empty() || has_node;
                }
                else if (thread_field == 4 && thread_wire == 2) {
                    has_positive_root_weight = packedDoubleHasPositive(thread.readString()) || has_positive_root_weight;
                }
                else if (thread_field == 5 && thread_wire == 2) {
                    has_root_refs = !thread.readString().empty() || has_root_refs;
                }
                else {
                    thread.skip(thread_wire);
                }
            }
            non_empty_thread_tree = non_empty_thread_tree || (has_node && has_positive_root_weight && has_root_refs);
            continue;
        }
        data.skip(wire_type);
    }
    return data.valid() && allocation_mode && non_empty_thread_tree;
}

#if defined(_WIN32) || defined(__linux__)
#ifdef _WIN32
#define SPARK_NOINLINE __declspec(noinline)
#else
#define SPARK_NOINLINE __attribute__((noinline))
#endif
SPARK_NOINLINE bool exerciseNativeAllocations()
{
    for (std::size_t i = 0; i < 4096; ++i) {
        const std::size_t size = 512 + (i & 255);
        void *allocation = std::malloc(size);
        if (allocation == nullptr) {
            return false;
        }
        static_cast<volatile unsigned char *>(allocation)[0] = static_cast<unsigned char>(i);
        std::free(allocation);
    }
    void *resized = std::malloc(1024);
    if (resized == nullptr) {
        return false;
    }
    void *replacement = std::realloc(resized, 4096);
    if (replacement == nullptr) {
        std::free(resized);
        return false;
    }
    std::free(replacement);

#ifdef _WIN32
    void *recalloced = _recalloc(nullptr, 32, 32);
    if (recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _recalloc failed\n");
        return false;
    }
    void *recalloced_replacement = _recalloc(recalloced, 64, 32);
    if (recalloced_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: resized _recalloc failed\n");
        std::free(recalloced);
        return false;
    }
    std::free(recalloced_replacement);

    void *aligned = _aligned_malloc(1024, 64);
    if (aligned == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_malloc failed\n");
        return false;
    }
    void *aligned_replacement = _aligned_realloc(aligned, 4096, 64);
    if (aligned_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_realloc failed\n");
        _aligned_free(aligned);
        return false;
    }
    void *aligned_recalloced = _aligned_recalloc(aligned_replacement, 128, 64, 64);
    if (aligned_recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_recalloc failed\n");
        _aligned_free(aligned_replacement);
        return false;
    }
    _aligned_free(aligned_recalloced);

    void *offset = _aligned_offset_malloc(1024, 64, 16);
    if (offset == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_malloc failed\n");
        return false;
    }
    void *offset_replacement = _aligned_offset_realloc(offset, 4096, 64, 16);
    if (offset_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_realloc failed\n");
        _aligned_free(offset);
        return false;
    }
    void *offset_recalloced = _aligned_offset_recalloc(offset_replacement, 128, 64, 64, 16);
    if (offset_recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_recalloc failed\n");
        _aligned_free(offset_replacement);
        return false;
    }
    _aligned_free(offset_recalloced);
#elif defined(__linux__)
    void *array = ::reallocarray(nullptr, 32, 32);
    if (array == nullptr) {
        return false;
    }
    void *array_replacement = ::reallocarray(array, 64, 32);
    if (array_replacement == nullptr) {
        std::free(array);
        return false;
    }
    std::free(array_replacement);
#endif

    void *cross_thread = std::malloc(4096);
    if (cross_thread == nullptr) {
        return false;
    }
    std::thread releaser([cross_thread]() { std::free(cross_thread); });
    releaser.join();
    return true;
}

bool verifyPersistentAllocationExportOrdering()
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

    const std::uint64_t persistent_before_direct_recovery = profiler.persistentAllocationBytes();
    const auto direct_recovery_advanced = [&] {
        return profiler.persistentAllocationBytes() > persistent_before_direct_recovery;
    };
    if (!exerciseNativeAllocations() || !waitForCondition(direct_recovery_advanced, 2s)) {
        std::fprintf(stderr, "persistent export: initial count-only statistics did not advance\n");
        return false;
    }
    const std::uint64_t persistent_at_direct_stop = profiler.persistentAllocationBytes();
    spark::ProfilerTestAccess::requestPersistentAllocationBackendStop(profiler);
    if (!profiler.resumePersistentAllocationCounting(error) ||
        !spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) ||
        !spark::ProfilerTestAccess::allocationSamplerRunning(profiler) ||
        profiler.persistentAllocationBytes() < persistent_at_direct_stop) {
        std::fprintf(stderr, "persistent export: direct stopped count-only recovery failed: %s\n", error.c_str());
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
    if (!profiler.resumePersistentAllocationCounting(error) ||
        spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: resume was not a no-op during full profile\n");
        profiler.cancel(error);
        return false;
    }
    if (!exerciseNativeAllocations() || !waitForCondition([&] { return profiler.sampleCount() != 0; }, 2s)) {
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
    const std::uint64_t samples_after_stop = profiler.sampleCount();
    const std::uint64_t bytes_after_stop = profiler.sampledAllocationBytes();
    if (spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) || samples_after_stop == 0 ||
        bytes_after_stop == 0 || samples_after_stop < samples_before_stop || bytes_after_stop < bytes_before_stop) {
        std::fprintf(stderr, "persistent export: completed profile was lost before export\n");
        return false;
    }

    std::string restart_error;
    if (profiler.start(options, server_tid, restart_error)) {
        std::fprintf(stderr, "persistent export: profiler restarted before completed allocation export\n");
        profiler.cancel(error);
        return false;
    }
    if (!profiler.setPersistentAllocationCountingEnabled(true, server_tid, error) ||
        spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) ||
        profiler.sampleCount() != samples_after_stop || profiler.sampledAllocationBytes() != bytes_after_stop) {
        std::fprintf(stderr, "persistent export: enabling count-only reset a completed unexported profile\n");
        return false;
    }

    const std::string profile = profiler.exportData({});
    if (profile.empty() || !serializedAllocationTreeNonEmpty(profile) || profiler.sampleCount() != samples_after_stop ||
        profiler.sampledAllocationBytes() != bytes_after_stop ||
        !metadataUnsigned(profile, "Allocation terminal in-flight tick samples discarded",
                          spark::ProfilerTestAccess::allocationTerminalSamples(profiler)) ||
        !metadataUnsigned(profile, "Allocation pending final drops",
                          spark::ProfilerTestAccess::allocationTerminalSamples(profiler)) ||
        !metadataUnsigned(profile, "Allocation samples dropped",
                          spark::ProfilerTestAccess::allocationDroppedSamples(profiler)) ||
        !metadataUnsigned(profile, "Allocation pending samples dropped",
                          spark::ProfilerTestAccess::allocationPendingSamples(profiler)) ||
        !metadataBoolean(profile, "Allocation data incomplete",
                         spark::ProfilerTestAccess::allocationDataIncomplete(profiler))) {
        std::fprintf(stderr,
                     "persistent export: serialized SamplerData did not preserve a non-empty allocation tree\n");
        return false;
    }
    if (!profiler.resumePersistentAllocationCounting(error) ||
        !spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) ||
        !profiler.resumePersistentAllocationCounting(error)) {
        std::fprintf(stderr, "persistent export: count-only resume/idempotence failed: %s\n", error.c_str());
        return false;
    }

    const std::uint64_t persistent_before_backend_stop = profiler.persistentAllocationBytes();
    const auto backend_stop_advanced = [&] {
        return profiler.persistentAllocationBytes() > persistent_before_backend_stop;
    };
    if (!exerciseNativeAllocations() || !waitForCondition(backend_stop_advanced, 2s)) {
        std::fprintf(stderr, "persistent export: pre-stop count-only statistics did not advance\n");
        return false;
    }
    const std::uint64_t persistent_at_backend_stop = profiler.persistentAllocationBytes();
    spark::ProfilerTestAccess::requestPersistentAllocationBackendStop(profiler);
    profiler.onTick(50.0);
    if (!profiler.persistentAllocationCountingEnabled() ||
        spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) ||
        profiler.persistentAllocationBytes() < persistent_at_backend_stop) {
        std::fprintf(stderr, "persistent export: transient count-only backend stop lost configured metrics/bytes\n");
        return false;
    }
    profiler.onTick(50.0);
    if (!spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) ||
        !spark::ProfilerTestAccess::allocationSamplerRunning(profiler)) {
        std::fprintf(stderr, "persistent export: idle tick did not restart stopped count-only backend\n");
        return false;
    }

    const std::uint64_t persistent_before = profiler.persistentAllocationBytes();
    if (!exerciseNativeAllocations() ||
        !waitForCondition([&] { return profiler.persistentAllocationBytes() > persistent_before; }, 2s)) {
        std::fprintf(stderr, "persistent export: resumed count-only statistics did not advance\n");
        return false;
    }

    if (!profiler.start(options, server_tid, error) ||
        spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) || !exerciseNativeAllocations() ||
        !waitForCondition([&] { return profiler.sampleCount() != 0; }, 2s) || !profiler.stopSampling(error)) {
        std::fprintf(stderr, "persistent export: second full allocation cycle failed: %s\n", error.c_str());
        return false;
    }
    const std::string second_profile = profiler.exportData({});
    if (!serializedAllocationTreeNonEmpty(second_profile) || !profiler.resumePersistentAllocationCounting(error) ||
        !spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: second serialized tree/resume cycle failed: %s\n", error.c_str());
        return false;
    }

    if (!profiler.setPersistentAllocationCountingEnabled(false, server_tid, error) ||
        profiler.persistentAllocationCountingEnabled() ||
        spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler) ||
        !profiler.resumePersistentAllocationCounting(error) ||
        spark::ProfilerTestAccess::persistentAllocationCountingActive(profiler)) {
        std::fprintf(stderr, "persistent export: disabled resume was not a no-op: %s\n", error.c_str());
        return false;
    }
    return profiler.shutdown(error);
}

bool verifyRetainedAllocationProfile()
{
    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.alloc_live_only = true;
    options.allocation_interval_bytes = 1;
    std::string error;
    const std::uint64_t server_tid = spark::currentNativeThreadId();
    if (!profiler.start(options, server_tid, error)) {
        std::fprintf(stderr, "retained allocation: start failed: %s\n", error.c_str());
        return false;
    }

    void *retained = std::malloc(8192);
    void *released = std::malloc(4096);
    if (retained == nullptr || released == nullptr) {
        std::free(retained);
        std::free(released);
        return false;
    }
    static_cast<volatile unsigned char *>(retained)[0] = 1;
    static_cast<volatile unsigned char *>(released)[0] = 2;
    void *resized = std::realloc(retained, 16384);
    if (resized != nullptr) {
        retained = resized;
    }
    void *failed_resize = std::realloc(retained, std::numeric_limits<std::size_t>::max());
    if (failed_resize != nullptr) {
        std::free(failed_resize);
        std::free(released);
        return false;
    }
    static_cast<volatile unsigned char *>(retained)[0] = 3;
    std::free(released);
    profiler.onTick(50.0);
    if (!profiler.stopSampling(error)) {
        std::fprintf(stderr, "retained allocation: stop failed: %s\n", error.c_str());
        std::free(retained);
        return false;
    }

    spark::ExportContext context;
    const std::string profile = profiler.exportData(context);
    const bool valid = profiler.sampleCount() != 0 && profiler.sampledAllocationBytes() >= 8192 &&
                       profiler.freedAllocationSamples() != 0 &&
                       profile.find("Allocation live-only") != std::string::npos &&
                       profile.find("Allocation retained maximum age ms") != std::string::npos &&
                       profile.find("Allocation hook capabilities") != std::string::npos &&
                       profile.find("Allocation hook targets installed") != std::string::npos &&
                       profile.find("Allocation thread filter stage") != std::string::npos;
    std::free(retained);
    if (!profiler.shutdown(error) || !valid) {
        std::fprintf(stderr,
                     "retained allocation: profile validation failed: %s "
                     "(samples=%llu bytes=%llu freed=%llu live-meta=%d age-meta=%d)\n",
                     error.c_str(), static_cast<unsigned long long>(profiler.sampleCount()),
                     static_cast<unsigned long long>(profiler.sampledAllocationBytes()),
                     static_cast<unsigned long long>(profiler.freedAllocationSamples()),
                     static_cast<int>(profile.find("Allocation live-only") != std::string::npos),
                     static_cast<int>(profile.find("Allocation retained maximum age ms") != std::string::npos));
        return false;
    }
    return true;
}

bool verifyAllocationLiveExport()
{
    using namespace std::chrono_literals;

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 4096;
    std::string error;
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "allocation live export: start failed: %s\n", error.c_str());
        return false;
    }

    void *first_allocation = std::malloc(4096);
    if (first_allocation == nullptr) {
        profiler.cancel(error);
        return false;
    }
    static_cast<volatile unsigned char *>(first_allocation)[0] = 1;
    if (!waitForCondition([&] { return profiler.sampleCount() != 0; }, 2s)) {
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }

    spark::AllocationSnapshot first;
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, first, error) || first.sample_count == 0 ||
        first.sampled_bytes == 0) {
        std::fprintf(stderr, "allocation live export: first snapshot failed: %s\n", error.c_str());
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }

    void *second_allocation = std::malloc(8192);
    if (second_allocation == nullptr) {
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }
    static_cast<volatile unsigned char *>(second_allocation)[0] = 2;
    if (!waitForCondition([&] { return profiler.sampleCount() > first.sample_count; }, 2s)) {
        std::free(second_allocation);
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }

    spark::AllocationSnapshot second;
    const std::string live_profile = profiler.liveExport({});
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, second, error) || live_profile.empty() ||
        live_profile.find("Allocation backend") == std::string::npos || second.sample_count < first.sample_count ||
        second.sampled_bytes < first.sampled_bytes || !spark::ProfilerTestAccess::allocationSamplerRunning(profiler) ||
        !spark::ProfilerTestAccess::allocationHooksInstalled(profiler)) {
        std::fprintf(stderr, "allocation live export: cumulative snapshot or sampler state was invalid\n");
        std::free(second_allocation);
        std::free(first_allocation);
        profiler.cancel(error);
        return false;
    }

    if (!profiler.stopSampling(error)) {
        std::free(second_allocation);
        std::free(first_allocation);
        return false;
    }
    const std::string final_profile = profiler.exportData({});
    const bool valid = !final_profile.empty() && profiler.sampleCount() >= second.sample_count &&
                       profiler.sampledAllocationBytes() >= second.sampled_bytes &&
                       spark::ProfilerTestAccess::allocationHooksInstalled(profiler);
    std::free(second_allocation);
    std::free(first_allocation);
    return profiler.shutdown(error) && valid;
}

bool verifyRetainedAllocationLiveExport()
{
    using namespace std::chrono_literals;

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.alloc_live_only = true;
    options.allocation_interval_bytes = 1;
    std::string error;
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "retained live export: start failed: %s\n", error.c_str());
        return false;
    }

    void *retained = std::malloc(1024 * 1024);
    void *released = std::malloc(512 * 1024);
    if (retained == nullptr || released == nullptr) {
        std::free(retained);
        std::free(released);
        profiler.cancel(error);
        return false;
    }
    static_cast<volatile unsigned char *>(retained)[0] = 1;
    static_cast<volatile unsigned char *>(released)[0] = 2;
    if (!waitForCondition([&] { return profiler.liveAllocationSamples() >= 2; }, 2s)) {
        std::fprintf(stderr, "retained live export: allocations were not tracked\n");
        std::free(released);
        std::free(retained);
        profiler.cancel(error);
        return false;
    }

    spark::AllocationSnapshot before_free;
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, before_free, error) || before_free.sample_count < 2) {
        std::fprintf(stderr, "retained live export: initial snapshot failed: %s (samples=%llu)\n", error.c_str(),
                     static_cast<unsigned long long>(before_free.sample_count));
        std::free(released);
        std::free(retained);
        profiler.cancel(error);
        return false;
    }
    std::free(released);
    if (!waitForCondition([&] { return profiler.freedAllocationSamples() != 0; }, 2s)) {
        std::fprintf(stderr, "retained live export: free was not tracked\n");
        std::free(retained);
        profiler.cancel(error);
        return false;
    }

    spark::AllocationSnapshot after_free;
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, after_free, error) ||
        after_free.sampled_bytes >= before_free.sampled_bytes) {
        std::fprintf(stderr, "retained live export: free was not reflected\n");
        std::free(retained);
        profiler.cancel(error);
        return false;
    }

    void *resized = std::realloc(retained, 2 * 1024 * 1024);
    if (resized == nullptr) {
        std::fprintf(stderr, "retained live export: realloc failed\n");
        std::free(retained);
        profiler.cancel(error);
        return false;
    }
    retained = resized;
    spark::AllocationSnapshot after_realloc;
    spark::AllocationSnapshot repeated;
    const std::string live_profile = profiler.liveExport({});
    if (!spark::ProfilerTestAccess::allocationSnapshot(profiler, after_realloc, error) ||
        !spark::ProfilerTestAccess::allocationSnapshot(profiler, repeated, error) || live_profile.empty() ||
        live_profile.find("Allocation live-only") == std::string::npos ||
        after_realloc.sampled_bytes < after_free.sampled_bytes || repeated.sample_count != after_realloc.sample_count ||
        repeated.sampled_bytes != after_realloc.sampled_bytes ||
        !spark::ProfilerTestAccess::allocationSamplerRunning(profiler) ||
        !spark::ProfilerTestAccess::allocationHooksInstalled(profiler)) {
        std::fprintf(stderr, "retained live export: realloc or repeated snapshot was invalid\n");
        std::free(retained);
        profiler.cancel(error);
        return false;
    }

    if (!profiler.stopSampling(error)) {
        std::fprintf(stderr, "retained live export: stop failed: %s (lifecycle=%llu, contention=%llu)\n", error.c_str(),
                     static_cast<unsigned long long>(spark::ProfilerTestAccess::allocationLifecycleDropped(profiler)),
                     static_cast<unsigned long long>(spark::ProfilerTestAccess::allocationContentionDropped(profiler)));
        std::free(retained);
        return false;
    }
    const std::string final_profile = profiler.exportData({});
    const bool valid = !final_profile.empty() && profiler.sampleCount() == repeated.sample_count &&
                       profiler.sampledAllocationBytes() == repeated.sampled_bytes;
    std::free(retained);
    return profiler.shutdown(error) && valid;
}
#endif

}  // namespace

int main()
{
    if (!verifyTerminalMetadataExport()) {
        return 1;
    }
#if defined(_WIN32) || defined(__linux__)
    if (!verifyTerminalMetadataExportWithSamples()) {
        return 1;
    }
#endif
#if defined(_WIN32) || defined(__linux__)
    if (!verifyPersistentAllocationExportOrdering()) {
        return 1;
    }
#endif
#ifdef __linux__
    if (!verifyRetainedAllocationProfile() || !verifyAllocationLiveExport() || !verifyRetainedAllocationLiveExport()) {
        return 1;
    }
#endif
    return 0;
}
