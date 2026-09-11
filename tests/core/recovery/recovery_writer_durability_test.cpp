#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>

#include <sys/wait.h>
#endif

#include "core/recovery/journal_reader.h"
#include "core/recovery/recovery_writer.h"

namespace spark {

struct RecoveryWriterQueueTestAccess {
    static bool dirty(const RecoveryWriter &writer) { return writer.dirty_; }
    static auto lastSync(const RecoveryWriter &writer) { return writer.last_sync_; }
    static bool snapshot(RecoveryWriter &writer) { return writer.writeMetadataSnapshot(); }
    static void reserve(RecoveryWriter &writer)
    {
        writer.active_producers_.fetch_add(1);
        writer.queue_size_.fetch_add(1);
    }
    static void publishReservedTick(RecoveryWriter &writer, std::uint64_t tick)
    {
        const auto sequence = writer.sequence_.fetch_add(1);
        assert(
            writer.queue_.enqueue(serializeRecord(RecordType::TickEvent, sequence, buildTickEventPayload(tick, 5.0))));
        writer.producerDone();
    }
    static void cancelReservation(RecoveryWriter &writer)
    {
        writer.queue_size_.fetch_sub(1);
        writer.producerDone();
    }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;
using Operation = spark::RecoveryWriter::IoOperation;

class Gate {
public:
    void enter()
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [&] { return released_; });
    }
    void waitEntered()
    {
        std::unique_lock lock(mutex_);
        assert(cv_.wait_for(lock, 3s, [&] { return entered_; }));
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
    bool entered_ = false;
    bool released_ = false;
};

template <typename Predicate>
void waitFor(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    assert(predicate());
}

std::filesystem::path testDirectory(const std::string &name)
{
    const auto directory = std::filesystem::temp_directory_path() / "spark_writer_durability" / name;
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

spark::RecoveryWriter::Config configFor(const std::string &name)
{
    spark::RecoveryWriter::Config config;
    config.directory = testDirectory(name);
    config.session_id = 1000;
    config.flush_interval_ms = 10;
    config.sync_interval_ms = 60000;
    return config;
}

void assertRecords(const std::filesystem::path &directory, std::uint64_t count)
{
    const auto journal = spark::JournalReader::readSession(directory);
    assert(journal.valid);
    assert(!journal.fatal_error);
    assert(journal.record_count == count);
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t tick = 0;
        double mspt = 0;
        assert(journal.records.at(static_cast<std::size_t>(i)).asTickEvent(tick, mspt));
        assert(tick == i + 1);
        assert(mspt == 5.0);
    }
}

void testCompletedOrdering()
{
    auto config = configFor("ordering");
    std::vector<Operation> completed;
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::WriteComplete || operation == Operation::FlushComplete ||
            operation == Operation::SyncComplete || operation == Operation::CloseComplete ||
            operation == Operation::RenameComplete) {
            completed.push_back(operation);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(1, 5.0);
    assert(writer.stop(3s));
    const std::vector<Operation> expected{
        Operation::WriteComplete, Operation::FlushComplete,  Operation::SyncComplete,
        Operation::CloseComplete, Operation::RenameComplete, Operation::WriteComplete,
        Operation::FlushComplete, Operation::SyncComplete,   Operation::CloseComplete};
    assert(completed == expected);
    assertRecords(config.directory, 1);
}

void testRunningFlush(bool explicit_request)
{
    auto config = configFor(explicit_request ? "explicit" : "idle");
    config.flush_interval_ms = explicit_request ? 60000 : 5;
    config.sync_interval_ms = explicit_request ? 60000 : 150;
    std::atomic<int> completed_syncs{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::SyncComplete) {
            completed_syncs.fetch_add(1);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(1, 5.0);
    if (explicit_request) {
        writer.requestFlush();
    }
    waitFor([&] { return writer.writtenRecords() == 1; });
    if (!explicit_request) {
        assert(completed_syncs.load() == 1);
    }
    waitFor([&] { return completed_syncs.load() == 2; });
    assert(writer.enabled());
    assert(!writer.stopRequested());
    assert(!writer.workerExited());
    assertRecords(config.directory, 1);
    assert(writer.stop(3s));
}

void testRequestDuringSync()
{
    auto config = configFor("request-during-sync");
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    std::atomic<int> completed_syncs{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::Sync && completed_syncs.load() == 1) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        }
        if (operation == Operation::SyncComplete) {
            completed_syncs.fetch_add(1);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(1, 5.0);
    writer.requestFlush();
    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 3s, [&] { return entered; }));
    }
    writer.journalTickEvent(2, 5.0);
    writer.requestFlush();
    {
        std::scoped_lock lock(mutex);
        released = true;
        cv.notify_all();
    }
    waitFor([&] { return completed_syncs.load() == 3; });
    assert(writer.enabled());
    assert(!writer.stopRequested());
    assertRecords(config.directory, 2);
    assert(writer.stop(3s));
}

void testFlushCoversBacklog()
{
    auto config = configFor("flush-backlog");
    config.flush_interval_ms = 60000;
    Gate gate;
    std::atomic<int> syncs{0};
    std::atomic<std::uint64_t> writes{0};
    std::atomic<std::uint64_t> synced_records{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::DrainComplete && syncs.load() == 1 && writes.load() == 0) {
            gate.enter();
        }
        if (operation == Operation::WriteComplete && syncs.load() != 0) {
            writes.fetch_add(1);
        }
        if (operation == Operation::SyncComplete) {
            synced_records.store(writes.load());
            syncs.fetch_add(1);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    spark::RecoveryWriterQueueTestAccess::reserve(writer);
    assert(writer.start());
    gate.waitEntered();
    for (std::uint64_t tick = 1; tick <= 600; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    spark::RecoveryWriterQueueTestAccess::cancelReservation(writer);
    writer.requestFlush();
    gate.release();
    waitFor([&] { return synced_records.load() == 600; });
    assert(syncs.load() >= 4);
    assert(writer.enabled());
    assert(!writer.stopRequested());
    assertRecords(config.directory, 600);
    assert(writer.stop(3s));
    std::cout << "M1 pre-request 600-record backlog: PASS\n";
}

void testUnpublishedReservationDoesNotResync()
{
    auto config = configFor("flush-reservation");
    Gate write_gate;
    Gate drain_gate;
    std::atomic<int> syncs{0};
    std::atomic<int> empty_batches{0};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::Write && syncs.load() == 1) {
            write_gate.enter();
        }
        if (operation == Operation::SyncComplete) {
            syncs.fetch_add(1);
        }
        if (operation == Operation::DrainComplete && syncs.load() >= 2 && empty_batches.fetch_add(1) == 100) {
            drain_gate.enter();
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    spark::RecoveryWriterQueueTestAccess::reserve(writer);
    writer.journalTickEvent(1, 5.0);
    write_gate.waitEntered();
    writer.requestFlush();
    write_gate.release();
    drain_gate.waitEntered();
    assert(syncs.load() == 2);
    spark::RecoveryWriterQueueTestAccess::publishReservedTick(writer, 2);
    drain_gate.release();
    waitFor([&] { return syncs.load() == 3; });
    assert(writer.enabled());
    assertRecords(config.directory, 2);
    assert(writer.stop(3s));
    std::cout << "M1 unpublished reservation, 100 empty batches without sync churn: PASS\n";
}

void testRequestAtWaitBoundary()
{
    auto config = configFor("wait-boundary");
    config.flush_interval_ms = 60000;
    Gate wait_gate;
    Gate request_gate;
    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    bool request_returned = false;
    std::atomic<int> syncs{0};
    std::atomic<bool> written{false};
    std::atomic<bool> armed{false};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::WriteComplete && syncs.load() == 1) {
            written.store(true);
        }
        if (operation == Operation::SyncComplete) {
            syncs.fetch_add(1);
        }
        if (operation == Operation::WaitBeforePark && written.load()) {
            wait_gate.enter();
        }
        if (operation == Operation::FlushRequestBeforeLock && armed.load()) {
            request_gate.enter();
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    spark::RecoveryWriterQueueTestAccess::reserve(writer);
    assert(writer.start());
    spark::RecoveryWriterQueueTestAccess::publishReservedTick(writer, 1);
    wait_gate.waitEntered();
    armed.store(true);
    std::thread requester([&] {
        writer.requestFlush();
        std::scoped_lock lock(completion_mutex);
        request_returned = true;
        completion_cv.notify_all();
    });
    request_gate.waitEntered();
    request_gate.release();
    {
        std::unique_lock lock(completion_mutex);
        assert(!completion_cv.wait_for(lock, 100ms, [&] { return request_returned; }));
    }
    wait_gate.release();
    {
        std::unique_lock lock(completion_mutex);
        assert(completion_cv.wait_for(lock, 3s, [&] { return request_returned; }));
    }
    requester.join();
    waitFor([&] { return syncs.load() == 2; });
    assert(writer.enabled());
    assert(!writer.stopRequested());
    assertRecords(config.directory, 1);
    assert(writer.stop(3s));
    std::cout << "M2 false-predicate/park boundary with concurrent request: PASS\n";
}

void testFailedSyncState(Operation failure)
{
    auto config = configFor("failed-sync-" + std::to_string(static_cast<int>(failure)));
    std::atomic<bool> armed{false};
    std::atomic<int> sync_attempts{0};
    config.io_hook = [&](Operation operation) {
        if (armed.load() && operation == Operation::Sync) {
            sync_attempts.fetch_add(1);
        }
        return !armed.load() || operation != failure;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    const auto previous_sync = spark::RecoveryWriterQueueTestAccess::lastSync(writer);
    armed.store(true);
    writer.journalTickEvent(1, 5.0);
    writer.requestFlush();
    waitFor([&] { return writer.workerExited(); });
    assert(writer.tryReap());
    assert(spark::RecoveryWriterQueueTestAccess::dirty(writer));
    assert(spark::RecoveryWriterQueueTestAccess::lastSync(writer) == previous_sync);
    assert(sync_attempts.load() == (failure == Operation::Flush ? 0 : 1));
}

void testQueueCannotStarveSync()
{
    auto config = configFor("queue-sync-deadline");
    config.sync_interval_ms = 40;
    std::atomic<int> completed_syncs{0};
    std::atomic<bool> feeding{true};
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::SyncComplete) {
            completed_syncs.fetch_add(1);
        }
        if (operation == Operation::Write && completed_syncs.load() == 1) {
            std::this_thread::sleep_for(1ms);
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    std::thread producer([&] {
        while (feeding.load()) {
            writer.journalTickEvent(1, 5.0);
        }
    });
    waitFor([&] { return completed_syncs.load() >= 2; });
    assert(writer.enabled());
    assert(!writer.stopRequested());
    feeding.store(false);
    producer.join();
    assert(writer.stop(3s));
}

void testRollingPublication()
{
    auto config = configFor("rolling-publication");
    config.max_segment_bytes = 256;
    config.max_total_bytes = 512;
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalModuleDef(0, "bedrock_server");
    writer.journalThreadDef(1, 100, "Server thread");
    for (std::uint64_t i = 1; i <= 200; ++i) {
        writer.journalTickEvent(i, 5.0);
    }
    assert(writer.stop(3s));
    assert(!std::filesystem::exists(config.directory / "segment-0.jnl"));
    const auto journal = spark::JournalReader::readSession(config.directory);
    assert(journal.valid);
    assert(!journal.fatal_error);
    assert(!journal.head_truncated);
    if (!journal.metadata_snapshot) {
        std::abort();
    }
    assert(journal.metadata_snapshot->valid);
    assert(journal.metadata_snapshot->modules.size() == 1);
    assert(journal.metadata_snapshot->threads.size() == 1);
    assert(journal.record_count > 0);
    assert(journal.record_count < 200);
}

void testRotationFault(Operation failure, int occurrence)
{
    auto config = configFor("rotation-" + std::to_string(static_cast<int>(failure)) + "-" + std::to_string(occurrence));
    config.max_segment_bytes = 1;
    config.max_total_bytes = 1;
    std::atomic<bool> armed{false};
    int seen = 0;
    int writes = 0;
    int partials = 0;
    int closes = 0;
    int publications = 0;
    config.io_hook = [&](Operation operation) {
        if (!armed.load()) {
            return true;
        }
        writes += operation == Operation::WriteComplete ? 1 : 0;
        partials += operation == Operation::PartialWriteComplete ? 1 : 0;
        closes += operation == Operation::Close ? 1 : 0;
        publications += operation == Operation::RenameComplete ? 1 : 0;
        return operation != failure || ++seen != occurrence;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    armed.store(true);
    writer.journalTickEvent(1, 5.0);
    assert(writer.stop(3s));
    assert(seen >= occurrence);
    assert(std::filesystem::exists(config.directory / "segment-0.jnl"));
    assert(!std::filesystem::exists(config.directory / "metadata.snapshot"));
    assert(!std::filesystem::exists(config.directory / "metadata.snapshot.tmp"));
    assert(!std::filesystem::exists(config.directory / "segment-1.jnl.tmp"));
    const auto journal = spark::JournalReader::readSession(config.directory);
    assert(journal.valid);
    assert(!journal.fatal_error);
    const bool active_write_failed =
        (failure == Operation::Write || failure == Operation::PartialWrite) && occurrence == 1;
    assert(std::cmp_equal(journal.record_count, active_write_failed ? 0 : 1));
    assert(partials == (failure == Operation::PartialWrite ? 1 : 0));
    if (failure == Operation::PartialWrite) {
        assert(writes == occurrence - 1);
        if (occurrence == 1) {
            assert(std::filesystem::file_size(config.directory / "segment-0.jnl") > spark::kFileHeaderSize);
            assert(journal.tail_truncated);
        }
    }
    if (occurrence == 3 || (failure == Operation::Rename && occurrence == 2)) {
        assert(closes == 4);
        assert(publications == 1);
    }
    else if (failure == Operation::Reopen) {
        assert(closes == 2);
        assert(publications == 1);
    }
    else if (failure == Operation::Rename || occurrence == 2) {
        assert(closes == 2);
        assert(publications == 0);
    }
    else {
        assert(closes == 1);
        assert(publications == 0);
    }
}

void testSnapshotDoesNotAcknowledgeActiveData()
{
    auto config = configFor("snapshot-state");
    std::atomic<bool> fail_sync{false};
    config.io_hook = [&](Operation operation) {
        return !fail_sync.load() || operation != Operation::Sync;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    fail_sync.store(true);
    writer.journalTickEvent(1, 5.0);
    writer.requestFlush();
    waitFor([&] { return writer.workerExited(); });
    assert(writer.tryReap());
    const auto previous_sync = spark::RecoveryWriterQueueTestAccess::lastSync(writer);
    fail_sync.store(false);
    assert(spark::RecoveryWriterQueueTestAccess::snapshot(writer));
    assert(spark::RecoveryWriterQueueTestAccess::dirty(writer));
    assert(spark::RecoveryWriterQueueTestAccess::lastSync(writer) == previous_sync);
}

std::string readBytes(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void testPriorSnapshotSurvives(Operation failure)
{
    auto config = configFor("prior-snapshot-" + std::to_string(static_cast<int>(failure)));
    config.max_segment_bytes = 92;
    config.max_total_bytes = 150;
    Gate first_snapshot;
    std::atomic<bool> armed{false};
    int renames = 0;
    int failures = 0;
    int partials = 0;
    config.io_hook = [&](Operation operation) {
        if (!armed.load()) {
            if (operation == Operation::RenameComplete && ++renames == 4) {
                first_snapshot.enter();
            }
            return true;
        }
        if (operation == Operation::PartialWriteComplete) {
            ++partials;
        }
        const bool is_snapshot = std::filesystem::exists(config.directory / "metadata.snapshot.tmp");
        if (is_snapshot && operation == failure) {
            ++failures;
            return false;
        }
        return true;
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        writer.journalTickEvent(tick, 5.0);
    }
    writer.requestFlush();
    first_snapshot.waitEntered();
    const auto snapshot = readBytes(config.directory / "metadata.snapshot");
    const auto retained = readBytes(config.directory / "segment-1.jnl");
    assert(!snapshot.empty());
    armed.store(true);
    writer.journalTickEvent(5, 5.0);
    writer.journalTickEvent(6, 5.0);
    first_snapshot.release();
    assert(writer.stop(3s));
    assert(failures == 1);
    assert(partials == (failure == Operation::PartialWrite ? 1 : 0));
    assert(readBytes(config.directory / "metadata.snapshot") == snapshot);
    assert(readBytes(config.directory / "segment-1.jnl") == retained);
    assert(std::filesystem::exists(config.directory / "segment-2.jnl"));
    assert(std::filesystem::exists(config.directory / "segment-3.jnl"));
    assert(!std::filesystem::exists(config.directory / "metadata.snapshot.tmp"));
    const auto journal = spark::JournalReader::readSession(config.directory);
    assert(journal.valid && !journal.fatal_error && !journal.head_truncated);
    assert(journal.record_count == 4);
}

[[noreturn]] void crashChild(const std::filesystem::path &directory, const std::string &point)
{
    spark::RecoveryWriter::Config config;
    config.directory = directory;
    config.session_id = 1000;
    config.flush_interval_ms = 5;
    config.sync_interval_ms = 60000;
    config.max_segment_bytes =
        spark::kFileHeaderSize +
        2 * spark::serializeRecord(spark::RecordType::TickEvent, 0, spark::buildTickEventPayload(1, 5.0)).size();
    std::atomic<int> syncs{0};
    std::atomic<bool> rotating{false};
    int writes = 0;
    config.io_hook = [&](Operation operation) {
        if (operation == Operation::SyncComplete) {
            syncs.fetch_add(1);
        }
        if (!rotating.load()) {
            return true;
        }
        if (operation == Operation::Write) {
            ++writes;
        }
        if ((point == "before-header" && operation == Operation::Write && writes == 2) ||
            (point == "partial-header" && writes == 2 && operation == Operation::CloseComplete) ||
            (point == "before-publish" && operation == Operation::Rename) ||
            (point == "after-publish" && operation == Operation::RenameComplete)) {
            std::_Exit(73);
        }
        return !(point == "partial-header" && writes == 2 && operation == Operation::PartialWrite);
    };
    spark::RecoveryWriter writer(config);
    assert(writer.start());
    writer.journalTickEvent(1, 5.0);
    writer.requestFlush();
    waitFor([&] { return syncs.load() == 2; });
    if (point == "durable-record") {
        std::_Exit(73);
    }
    rotating.store(true);
    writer.journalTickEvent(2, 5.0);
    writer.requestFlush();
    std::this_thread::sleep_for(3s);
    std::_Exit(74);
}

void testCrash(const char *executable, const std::string &point)
{
    const auto directory = testDirectory("crash-" + point);
    const auto path = directory.string();
#ifdef _WIN32
    const char *arguments[]{executable, "--child", path.c_str(), point.c_str(), nullptr};
    assert(_spawnv(_P_WAIT, executable, arguments) == 73);
#else
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(executable, executable, "--child", path.c_str(), point.c_str(), nullptr);
        std::_Exit(75);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 73);
#endif
    assertRecords(directory, point == "durable-record" ? 1 : 2);
    assert(std::filesystem::exists(directory / "segment-1.jnl") == (point == "after-publish"));
    if (point == "partial-header") {
        assert(std::filesystem::file_size(directory / "segment-1.jnl.tmp") == spark::kFileHeaderSize / 2);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--child") {
        crashChild(argv[2], argv[3]);
    }
    const std::string group = argc == 2 ? argv[1] : "all";
    if (group == "all" || group == "--durability") {
        testCompletedOrdering();
        testRollingPublication();
        for (const auto operation :
             {Operation::Write, Operation::PartialWrite, Operation::Flush, Operation::Sync, Operation::Close}) {
            for (int occurrence = 1; occurrence <= 3; ++occurrence) {
                testRotationFault(operation, occurrence);
            }
        }
        testRotationFault(Operation::Rename, 1);
        testRotationFault(Operation::Rename, 2);
        testRotationFault(Operation::Reopen, 1);
        testSnapshotDoesNotAcknowledgeActiveData();
        for (const auto operation : {Operation::Write, Operation::PartialWrite, Operation::Flush, Operation::Sync,
                                     Operation::Close, Operation::Rename}) {
            testPriorSnapshotSurvives(operation);
        }
        std::cout << "P1#4 prior-good snapshot and retained segment preservation, six failures: PASS\n";
        for (const auto *point :
             {"durable-record", "before-header", "partial-header", "before-publish", "after-publish"}) {
            testCrash(argv[0], point);
        }
        std::cout << "P1#4 durability, rotation faults and subprocess crashes: PASS\n";
    }
    if (group == "all" || group == "--flush") {
        testRunningFlush(true);
        testRunningFlush(false);
        testRequestDuringSync();
        testFlushCoversBacklog();
        testUnpublishedReservationDoesNotResync();
        testRequestAtWaitBoundary();
        testQueueCannotStarveSync();
        testFailedSyncState(Operation::Flush);
        testFailedSyncState(Operation::Sync);
        std::cout << "P2#15 running explicit/idle/concurrent-request flush and failure state: PASS\n";
    }
}
