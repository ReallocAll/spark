#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "core/profiler/profiler.h"
#include "core/recovery/recovery_player.h"
#include "core/recovery/recovery_writer.h"
#include "native/alloc/allocation_lifecycle_test_access.h"
#include "native/sampler/thread_info.h"

namespace spark {

struct ProfilerLifecycleTestAccess {
    static void installRecoveryWriter(Profiler &profiler, std::unique_ptr<RecoveryWriter> writer)
    {
        std::scoped_lock lock(profiler.recovery_mutex_);
        profiler.recovery_writer_ = std::move(writer);
    }

    static bool hasRecoveryWriter(const Profiler &profiler)
    {
        std::scoped_lock lock(profiler.recovery_mutex_);
        return profiler.recovery_writer_ != nullptr;
    }

    static AllocationSampler &allocationSampler(Profiler &profiler) { return profiler.allocation_sampler_; }

    static bool allocationExportPending(const Profiler &profiler)
    {
        return profiler.allocation_export_pending_.load(std::memory_order_acquire);
    }

    static bool persistentCountingActive(const Profiler &profiler)
    {
        return profiler.persistent_allocation_counting_active_.load(std::memory_order_acquire);
    }

    static std::uint64_t persistentBytesBase(const Profiler &profiler)
    {
        return profiler.persistent_allocation_bytes_base_.load(std::memory_order_acquire);
    }

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

std::filesystem::path testRoot(std::string_view name)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("spark_profiler_recovery_lifecycle_" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    return root;
}

class IoGate {
public:
    using Operation = spark::RecoveryWriter::IoOperation;

    void arm(Operation operation)
    {
        std::scoped_lock lock(mutex_);
        operation_ = operation;
        armed_ = true;
        entered_ = false;
        released_ = false;
    }

    bool hook(Operation operation)
    {
        std::unique_lock lock(mutex_);
        if (!armed_ || operation != operation_) {
            return true;
        }
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return true;
    }

    bool waitEntered(std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release()
    {
        std::scoped_lock lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    Operation operation_ = Operation::Write;
    bool armed_ = false;
    bool entered_ = false;
    bool released_ = false;
};

std::filesystem::path makeJournal(const std::string &name, std::uint8_t profile_type = 0)
{
    const auto directory = testRoot(name) / "session";
    spark::RecoveryWriter::Config config;
    config.directory = directory;
    config.session_id = 1'000'000;
    config.flush_interval_ms = 10;
    config.sync_interval_ms = 10;
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalSessionConfig(4000, 0, false, false, false, 1, profile_type, false, "Console", false, "lifecycle",
                                {"Server thread"}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 2, "Server thread");
    spark::Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 1;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);
    writer.journalTickEvent(1, 5.0);
    assert(writer.stop(1s));
    return directory;
}

void testDiscardStatusesAndFilesystemTruth()
{
    spark::Profiler profiler;
    const auto root = testRoot("discard");
    profiler.setRecoveryDirectory(root);
    std::filesystem::create_directories(root / "session");

    auto result = profiler.discardRecoveryJournal();
    assert(result.status == spark::RecoveryDiscardStatus::Completed);
    assert(!std::filesystem::exists(root));

    std::filesystem::create_directories(root);
    std::ofstream(root / "sentinel") << "journal";
    spark::ProfilerLifecycleTestAccess::setRecoveryRemove(
        profiler, [](const std::filesystem::path &, std::error_code &error) {
            error = std::make_error_code(std::errc::permission_denied);
        });
    result = profiler.discardRecoveryJournal();
    assert(result.status == spark::RecoveryDiscardStatus::FilesystemError);
    assert(result.message.find("cleanup failed") != std::string::npos);
    assert(std::filesystem::exists(root / "sentinel"));
    assert(spark::ProfilerLifecycleTestAccess::recoveryRetained(profiler));

    spark::ProfilerLifecycleTestAccess::setRecoveryRemove(profiler, {});
    result = profiler.discardRecoveryJournal();
    assert(result.completed());
    assert(!std::filesystem::exists(root));
}

void testWriterPendingDiscardThenRetry()
{
    IoGate gate;
    const auto root = testRoot("writer-pending");
    spark::RecoveryWriter::Config config;
    config.directory = root;
    config.session_id = 2;
    config.shutdown_timeout_ms = 20;
    config.io_hook = [&gate](spark::RecoveryWriter::IoOperation operation) {
        return gate.hook(operation);
    };
    auto writer = std::make_unique<spark::RecoveryWriter>(config);
    assert(writer->start());
    gate.arm(spark::RecoveryWriter::IoOperation::Close);
    writer->requestStop();
    assert(gate.waitEntered());

    spark::Profiler profiler;
    profiler.setRecoveryDirectory(root);
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(writer));
    const auto pending = profiler.discardRecoveryJournal();
    assert(pending.status == spark::RecoveryDiscardStatus::WriterPending);
    assert(spark::ProfilerLifecycleTestAccess::hasRecoveryWriter(profiler));
    assert(std::filesystem::exists(root));

    gate.release();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (spark::ProfilerLifecycleTestAccess::hasRecoveryWriter(profiler) &&
           std::chrono::steady_clock::now() < deadline) {
        (void)profiler.discardRecoveryJournal();
        std::this_thread::yield();
    }
    const auto completed = profiler.discardRecoveryJournal();
    assert(completed.completed());
    assert(!spark::ProfilerLifecycleTestAccess::hasRecoveryWriter(profiler));
    assert(!std::filesystem::exists(root));
}

void testRetainedShutdownReapsWriterBeforeSuccess()
{
    IoGate gate;
    const auto root = testRoot("retained-shutdown");
    spark::RecoveryWriter::Config config;
    config.directory = root;
    config.session_id = 3;
    config.shutdown_timeout_ms = 20;
    config.io_hook = [&gate](spark::RecoveryWriter::IoOperation operation) {
        return gate.hook(operation);
    };
    auto writer = std::make_unique<spark::RecoveryWriter>(config);
    assert(writer->start());
    gate.arm(spark::RecoveryWriter::IoOperation::Close);
    writer->requestStop();
    assert(gate.waitEntered());

    spark::Profiler profiler;
    profiler.setRecoveryDirectory(root);
    profiler.retainRecoveryJournalOnShutdown();
    spark::ProfilerLifecycleTestAccess::installRecoveryWriter(profiler, std::move(writer));
    std::string error;
    assert(!profiler.shutdown(error));
    assert(error.find("recovery writer shutdown timed out") != std::string::npos);
    assert(spark::ProfilerLifecycleTestAccess::hasRecoveryWriter(profiler));

    gate.release();
    assert(profiler.shutdown(error));
    assert(!spark::ProfilerLifecycleTestAccess::hasRecoveryWriter(profiler));
    assert(std::filesystem::exists(root));
    assert(!spark::RecoveryPlayer::replay(root).has_clean_end);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void testPositiveRecoveryReplayAndCleanup()
{
    const auto execution_directory = makeJournal("execution-replay", 0);
    const auto execution = spark::RecoveryPlayer::replay(execution_directory);
    assert(execution.valid);
    assert(!execution.has_clean_end);
    assert(execution.sample_count == 1);

    spark::Profiler execution_profiler;
    execution_profiler.setRecoveryDirectory(execution_directory);
    assert(execution_profiler.discardRecoveryJournal().completed());
    assert(!std::filesystem::exists(execution_directory));

    const auto allocation_directory = makeJournal("allocation-replay", 1);
    const auto allocation = spark::RecoveryPlayer::replay(allocation_directory);
    assert(allocation.valid);
    assert(!allocation.has_clean_end);
    assert(allocation.sample_count == 1);

    spark::Profiler allocation_profiler;
    allocation_profiler.setRecoveryDirectory(allocation_directory);
    assert(allocation_profiler.discardRecoveryJournal().completed());
    assert(!std::filesystem::exists(allocation_directory));
}

void testBackendPendingBlocksBothModesAndPreservesState()
{
    spark::Profiler profiler;
    const auto root = testRoot("backend-pending");
    profiler.setRecoveryDirectory(root);
    std::ofstream(root / "sentinel") << "journal";
    std::string error;
    assert(profiler.setPersistentAllocationCountingEnabled(true, spark::currentNativeThreadId(), error));
    const spark::ProfilerOptions before_options = profiler.options();
    const spark::ProfileMode before_mode = profiler.mode();

    spark::test::TrackingGate tracking_gate;
    std::thread holder([&] {
        (void)spark::test::AllocationLifecycleTestAccess::holdTrackingCall(
            spark::ProfilerLifecycleTestAccess::allocationSampler(profiler), tracking_gate);
    });
    assert(waitFor([&] { return tracking_gate.entered.load(std::memory_order_acquire); }, 2s));

    assert(!profiler.setPersistentAllocationCountingEnabled(false, spark::currentNativeThreadId(), error));
    assert(profiler.persistentAllocationCountingEnabled());
    assert(spark::ProfilerLifecycleTestAccess::allocationSampler(profiler).backendCleanupPending());

    spark::ProfilerOptions execution_options;
    execution_options.interval_ms = 1;
    assert(!profiler.start(execution_options, spark::currentNativeThreadId(), error));
    assert(profiler.mode() == before_mode);
    assert(profiler.options().interval_ms == before_options.interval_ms);
    assert(!spark::ProfilerLifecycleTestAccess::hasRecoveryWriter(profiler));

    spark::ProfilerOptions allocation_options;
    allocation_options.alloc = true;
    allocation_options.allocation_interval_bytes = 1;
    assert(!profiler.start(allocation_options, spark::currentNativeThreadId(), error));
    assert(profiler.mode() == before_mode);
    assert(profiler.options().alloc == before_options.alloc);
    assert(!std::filesystem::exists(root) || std::filesystem::exists(root / "sentinel"));

    const auto pending = profiler.discardRecoveryJournal();
    assert(pending.status == spark::RecoveryDiscardStatus::BackendCleanupPending);
    assert(std::filesystem::exists(root / "sentinel"));

    tracking_gate.release.store(true, std::memory_order_release);
    holder.join();
    assert(tracking_gate.exited.load(std::memory_order_acquire));
    assert(profiler.setPersistentAllocationCountingEnabled(false, spark::currentNativeThreadId(), error));
    assert(!spark::ProfilerLifecycleTestAccess::allocationSampler(profiler).backendCleanupPending());
    assert(profiler.discardRecoveryJournal().completed());

    assert(profiler.start(allocation_options, spark::currentNativeThreadId(), error));
    assert(profiler.stopSampling(error));
    assert(!profiler.exportData({}).empty());
    assert(profiler.resumePersistentAllocationCounting(error));
    assert(profiler.start(execution_options, spark::currentNativeThreadId(), error));
    assert(profiler.stopSampling(error));
    assert(!profiler.exportData({}).empty());
    assert(profiler.shutdown(error));
}

void testFailedAllocationCancelResumesCounting()
{
    spark::Profiler profiler;
    std::string error;
    const auto thread_id = spark::currentNativeThreadId();
    assert(profiler.setPersistentAllocationCountingEnabled(true, thread_id, error));
    spark::ProfilerOptions options;
    options.alloc = true;
    options.fail_allocation_aggregator_for_testing = true;
    assert(profiler.start(options, thread_id, error));
    const auto base = spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler);
    assert(waitFor([&] { return profiler.backendFailure(error); }, 2s));
    assert(!profiler.stopSampling(error));
    assert(error.find("injected allocation aggregator failure") != std::string::npos);
    auto &sampler = spark::ProfilerLifecycleTestAccess::allocationSampler(profiler);
    assert(!sampler.running());
    assert(!sampler.backendCleanupPending());
    assert(!sampler.aggregatorMayBeAlive());
    assert(sampler.allocationDiagnostics().accounting_state == spark::AllocationAccountingState::Failed);
    assert(profiler.backendFailure(error));
    assert(spark::ProfilerLifecycleTestAccess::allocationExportPending(profiler));
    const auto expected_base = base + sampler.observedBytes();
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(profiler.cancel(error));
    assert(error.empty());
    assert(!profiler.running());
    assert(!spark::ProfilerLifecycleTestAccess::allocationExportPending(profiler));
    assert(spark::ProfilerLifecycleTestAccess::persistentCountingActive(profiler));
    assert(sampler.running());
    assert(!profiler.backendFailure(error));
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(profiler.resumePersistentAllocationCounting(error));
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(profiler.shutdown(error));
}

void testAllocationCancelRetainsPendingOwnership()
{
    spark::Profiler profiler;
    std::string error;
    const auto thread_id = spark::currentNativeThreadId();
    assert(profiler.setPersistentAllocationCountingEnabled(true, thread_id, error));
    spark::ProfilerOptions options;
    options.alloc = true;
    assert(profiler.start(options, thread_id, error));
    const auto base = spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler);
    auto &sampler = spark::ProfilerLifecycleTestAccess::allocationSampler(profiler);
    void *probe = std::malloc(4096);
    assert(probe != nullptr);
    static_cast<volatile unsigned char *>(probe)[0] = 1;
    std::free(probe);
    assert(sampler.observedBytes() >= 4096);
    spark::test::TrackingGate tracking_gate;
    std::thread holder(
        [&] { (void)spark::test::AllocationLifecycleTestAccess::holdTrackingCall(sampler, tracking_gate); });
    assert(waitFor([&] { return tracking_gate.entered.load(std::memory_order_acquire); }, 2s));
    assert(!profiler.cancel(error));
    assert(!error.empty());
    assert(!profiler.running());
    assert(sampler.backendCleanupPending());
    assert(spark::ProfilerLifecycleTestAccess::allocationExportPending(profiler));
    assert(!spark::ProfilerLifecycleTestAccess::persistentCountingActive(profiler));
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == base);
    assert(!profiler.start(options, thread_id, error));
    assert(!profiler.start({}, thread_id, error));
    tracking_gate.release.store(true, std::memory_order_release);
    holder.join();
    assert(tracking_gate.exited.load(std::memory_order_acquire));
    const auto expected_base = base + sampler.observedBytes();
    assert(profiler.cancel(error));
    assert(!sampler.backendCleanupPending());
    assert(!spark::ProfilerLifecycleTestAccess::allocationExportPending(profiler));
    assert(spark::ProfilerLifecycleTestAccess::persistentCountingActive(profiler));
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(profiler.resumePersistentAllocationCounting(error));
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(profiler.start(options, thread_id, error));
    assert(profiler.cancel(error));
    assert(profiler.shutdown(error));
}

#ifdef __linux__
std::atomic<bool> SnapshotConsumerEntered{false};
std::atomic<bool> ReleaseSnapshotConsumer{false};

void holdSnapshotConsumer() noexcept
{
    SnapshotConsumerEntered.store(true, std::memory_order_release);
    while (!ReleaseSnapshotConsumer.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void testFailedCountingStartPreservesRetiredBytes()
{
    spark::test::LinuxAllocationTestControl control;
    spark::Profiler profiler;
    std::string error;
    const auto thread_id = spark::currentNativeThreadId();
    assert(profiler.setPersistentAllocationCountingEnabled(true, thread_id, error));
    spark::ProfilerOptions options;
    options.alloc = true;
    assert(profiler.start(options, thread_id, error));
    const auto base = spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler);
    void *probe = std::malloc(4096);
    assert(probe != nullptr);
    static_cast<volatile unsigned char *>(probe)[0] = 1;
    std::free(probe);
    assert(profiler.stopSampling(error));
    auto &sampler = spark::ProfilerLifecycleTestAccess::allocationSampler(profiler);
    const auto retired_bytes = sampler.observedBytes();
    assert(retired_bytes >= 4096);
    const auto expected_base = base + retired_bytes;
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(!sampler.running());
    assert(!sampler.backendCleanupPending());
    assert(!sampler.aggregatorMayBeAlive());

    SnapshotConsumerEntered.store(false, std::memory_order_release);
    ReleaseSnapshotConsumer.store(false, std::memory_order_release);
    control.snapshot_admitted = holdSnapshotConsumer;
    assert(spark::test::AllocationLifecycleTestAccess::configureLinux(sampler, &control));
    bool snapshot_succeeded = true;
    std::thread consumer([&] {
        spark::AllocationSnapshot snapshot;
        std::string snapshot_error;
        snapshot_succeeded = sampler.snapshot(snapshot, snapshot_error);
    });
    assert(waitFor([&] { return SnapshotConsumerEntered.load(std::memory_order_acquire); }, 2s));
    assert(!profiler.resumePersistentAllocationCounting(error));
    assert(!error.empty());
    assert(sampler.backendCleanupPending());
    assert(!spark::ProfilerLifecycleTestAccess::persistentCountingActive(profiler));
    assert(sampler.observedBytes() == retired_bytes);
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(!profiler.resumePersistentAllocationCounting(error));
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    ReleaseSnapshotConsumer.store(true, std::memory_order_release);
    consumer.join();
    assert(!snapshot_succeeded);
    assert(sampler.observedBytes() == retired_bytes);
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(profiler.resumePersistentAllocationCounting(error));
    assert(!sampler.backendCleanupPending());
    assert(spark::ProfilerLifecycleTestAccess::persistentCountingActive(profiler));
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(profiler.resumePersistentAllocationCounting(error));
    assert(spark::ProfilerLifecycleTestAccess::persistentBytesBase(profiler) == expected_base);
    assert(profiler.shutdown(error));
}
#endif

}  // namespace

int main()
{
    testDiscardStatusesAndFilesystemTruth();
    testWriterPendingDiscardThenRetry();
    testRetainedShutdownReapsWriterBeforeSuccess();
    testPositiveRecoveryReplayAndCleanup();
    testBackendPendingBlocksBothModesAndPreservesState();
    testFailedAllocationCancelResumesCounting();
    testAllocationCancelRetainsPendingOwnership();
#ifdef __linux__
    testFailedCountingStartPreservesRetiredBytes();
#endif
    std::cout << "Profiler recovery lifecycle tests passed.\n";
    return 0;
}
