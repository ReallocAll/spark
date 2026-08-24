#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/profiler/profiler.h"
#include "core/recovery/recovery_player.h"
#include "core/recovery/recovery_writer.h"

namespace spark {

struct RecoveryWriterQueueTestAccess {
    static void enable(RecoveryWriter &writer) { writer.enabled_.store(true, std::memory_order_release); }

    static std::size_t queued(const RecoveryWriter &writer)
    {
        return writer.queue_size_.load(std::memory_order_relaxed);
    }
};

struct ProfilerTestAccess {
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
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;

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
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
        return true;
    }

    bool waitEntered(std::chrono::milliseconds timeout = 1s)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release()
    {
        std::scoped_lock lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    Operation operation_ = Operation::Write;
    bool armed_ = false;
    bool entered_ = false;
    bool released_ = false;
};

std::filesystem::path makeTempDir(const std::string &name)
{
    auto base = std::filesystem::temp_directory_path() / "spark_recovery_writer_queue_test" / name;
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base;
}

spark::RecoveryWriter::Config gatedConfig(const std::string &name, IoGate &gate)
{
    spark::RecoveryWriter::Config cfg;
    cfg.directory = makeTempDir(name);
    cfg.session_id = 1;
    cfg.flush_interval_ms = 100000;
    cfg.sync_interval_ms = 100000;
    cfg.shutdown_timeout_ms = 20;
    cfg.io_hook = [&gate](spark::RecoveryWriter::IoOperation operation) { return gate.hook(operation); };
    return cfg;
}

void assertBoundedTimeout(spark::RecoveryWriter &writer)
{
    const auto before = std::chrono::steady_clock::now();
    assert(!writer.stop(20ms));
    const auto elapsed = std::chrono::steady_clock::now() - before;
    assert(elapsed < 500ms);
    assert(writer.stopRequested());
    assert(!writer.workerExited());
}

void testWriterQueueDrop()
{
    auto dir = makeTempDir("queue-drop");
    spark::RecoveryWriter::Config cfg;
    cfg.directory = dir / "session-2";
    cfg.session_id = 1;
    cfg.flush_interval_ms = 100000;
    cfg.sync_interval_ms = 100000;
    cfg.queue_capacity = 10;

    spark::RecoveryWriter writer(cfg);
    assert(writer.start());

    for (int i = 0; i < 100; ++i) {
        writer.journalTickEvent(static_cast<std::uint64_t>(i), static_cast<double>(i));
    }

    assert(writer.stop(1s));
    assert(writer.droppedRecords() > 0);
    assert(writer.writtenRecords() <= cfg.queue_capacity);
    std::cout << "testWriterQueueDrop: PASS\n";
}

void testConcurrentQueueCapacity()
{
    constexpr std::size_t capacity = 7;
    constexpr int producer_count = 8;
    constexpr int attempts_per_producer = 1000;
    constexpr std::uint64_t total_attempts =
        static_cast<std::uint64_t>(producer_count) * static_cast<std::uint64_t>(attempts_per_producer);

    spark::RecoveryWriter::Config cfg;
    cfg.directory = makeTempDir("disabled-drain");
    cfg.queue_capacity = capacity;

    spark::RecoveryWriter writer(cfg);
    spark::RecoveryWriterQueueTestAccess::enable(writer);

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&writer, producer] {
            for (int attempt = 0; attempt < attempts_per_producer; ++attempt) {
                const auto tick_id =
                    static_cast<std::uint64_t>(producer) * static_cast<std::uint64_t>(attempts_per_producer) +
                    static_cast<std::uint64_t>(attempt);
                writer.journalTickEvent(tick_id, 1.0);
            }
        });
    }
    for (auto &producer : producers) {
        producer.join();
    }

    const auto accepted = spark::RecoveryWriterQueueTestAccess::queued(writer);
    const auto dropped = writer.droppedRecords();
    assert(accepted <= capacity);
    assert(accepted + dropped == total_attempts);
    assert(dropped > 0);
    assert(writer.stop(20ms));
    std::cout << "testConcurrentQueueCapacity: PASS\n";
}

void testBlockedWriteIsBoundedAndReapable()
{
    IoGate gate;
    auto cfg = gatedConfig("blocked-write", gate);
    spark::RecoveryWriter writer(cfg);
    assert(writer.start());

    gate.arm(spark::RecoveryWriter::IoOperation::Write);
    writer.journalTickEvent(1, 1.0);
    writer.requestFlush();
    assert(gate.waitEntered());

    assertBoundedTimeout(writer);
    const auto queued_after_stop = spark::RecoveryWriterQueueTestAccess::queued(writer);
    writer.journalTickEvent(2, 2.0);
    assert(spark::RecoveryWriterQueueTestAccess::queued(writer) == queued_after_stop);
    assert(!writer.tryReap());

    gate.release();
    assert(writer.stop(1s));
    assert(writer.workerExited());
    assert(writer.tryReap());
    std::cout << "testBlockedWriteIsBoundedAndReapable: PASS\n";
}

void testBlockedSyncIsBoundedAndReapable()
{
    IoGate gate;
    auto cfg = gatedConfig("blocked-sync", gate);
    spark::RecoveryWriter writer(cfg);
    assert(writer.start());

    gate.arm(spark::RecoveryWriter::IoOperation::Sync);
    writer.requestStop();
    assert(gate.waitEntered());
    assertBoundedTimeout(writer);

    gate.release();
    assert(writer.stop(1s));
    std::cout << "testBlockedSyncIsBoundedAndReapable: PASS\n";
}

void testBlockedCloseIsBoundedAndReapable()
{
    IoGate gate;
    auto cfg = gatedConfig("blocked-close", gate);
    spark::RecoveryWriter writer(cfg);
    assert(writer.start());

    gate.arm(spark::RecoveryWriter::IoOperation::Close);
    writer.requestStop();
    assert(gate.waitEntered());
    assertBoundedTimeout(writer);

    gate.release();
    assert(writer.stop(1s));
    std::cout << "testBlockedCloseIsBoundedAndReapable: PASS\n";
}

void testPostStopAdmissionClosed()
{
    spark::RecoveryWriter::Config cfg;
    cfg.directory = makeTempDir("post-stop-admission");
    cfg.session_id = 2;
    cfg.flush_interval_ms = 100000;
    cfg.sync_interval_ms = 100000;

    spark::RecoveryWriter writer(cfg);
    assert(writer.start());
    writer.requestStop();
    const auto queued = spark::RecoveryWriterQueueTestAccess::queued(writer);
    writer.journalTickEvent(1, 1.0);
    assert(spark::RecoveryWriterQueueTestAccess::queued(writer) == queued);
    assert(writer.stop(1s));
    std::cout << "testPostStopAdmissionClosed: PASS\n";
}

void testJournalPreservedAcrossTimeout()
{
    IoGate gate;
    auto cfg = gatedConfig("journal-preservation", gate);
    cfg.session_id = 950000;
    spark::RecoveryWriter writer(cfg);
    assert(writer.start());

    writer.journalSessionConfig(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0);
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");
    spark::Sample sample;
    sample.thread_id = 1;
    sample.tick_id = 0;
    sample.window = 0;
    sample.weight = 4000;
    sample.frames.push_back({.module = 0, .rva = 0x1000, .raw_address = 0});
    writer.journalSample(sample);
    writer.journalTickEvent(0, 5.0);

    gate.arm(spark::RecoveryWriter::IoOperation::Sync);
    writer.requestStop();
    assert(gate.waitEntered());
    assertBoundedTimeout(writer);
    assert(std::filesystem::exists(cfg.directory / "segment-0.jnl"));

    gate.release();
    assert(writer.stop(1s));
    const auto recovered = spark::RecoveryPlayer::replay(cfg.directory);
    assert(recovered.valid);
    assert(recovered.sample_count == 1);
    assert(!recovered.has_clean_end);
    std::cout << "testJournalPreservedAcrossTimeout: PASS\n";
}

void testRepeatedStopIsIdempotent()
{
    spark::RecoveryWriter::Config cfg;
    cfg.directory = makeTempDir("repeated-stop");
    cfg.session_id = 3;

    spark::RecoveryWriter writer(cfg);
    assert(writer.start());
    writer.journalTickEvent(1, 1.0);
    assert(writer.stop(1s));
    assert(writer.stop(20ms));
    assert(writer.tryReap());
    std::cout << "testRepeatedStopIsIdempotent: PASS\n";
}

void testProfilerShutdownFailsClosedWhileWriterLives()
{
    IoGate gate;
    auto cfg = gatedConfig("profiler-fail-closed", gate);
    auto writer = std::make_unique<spark::RecoveryWriter>(cfg);
    assert(writer->start());

    gate.arm(spark::RecoveryWriter::IoOperation::Close);
    writer->requestStop();
    assert(gate.waitEntered());

    spark::Profiler profiler;
    spark::ProfilerTestAccess::installRecoveryWriter(profiler, std::move(writer));
    std::string error;
    const auto before = std::chrono::steady_clock::now();
    assert(!profiler.shutdown(error));
    assert(std::chrono::steady_clock::now() - before < 500ms);
    assert(error.find("recovery writer shutdown timed out") != std::string::npos);
    assert(spark::ProfilerTestAccess::hasRecoveryWriter(profiler));

    gate.release();
    assert(profiler.shutdown(error));
    assert(!spark::ProfilerTestAccess::hasRecoveryWriter(profiler));
    std::cout << "testProfilerShutdownFailsClosedWhileWriterLives: PASS\n";
}

}  // namespace

int main()
{
    testWriterQueueDrop();
    testConcurrentQueueCapacity();
    testBlockedWriteIsBoundedAndReapable();
    testBlockedSyncIsBoundedAndReapable();
    testBlockedCloseIsBoundedAndReapable();
    testPostStopAdmissionClosed();
    testJournalPreservedAcrossTimeout();
    testRepeatedStopIsIdempotent();
    testProfilerShutdownFailsClosedWhileWriterLives();
    std::cout << "All recovery writer shutdown tests passed.\n";
    return 0;
}
