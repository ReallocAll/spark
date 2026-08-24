#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#include "core/recovery/recovery_writer.h"

namespace spark {

struct RecoveryWriterQueueTestAccess {
    static void enable(RecoveryWriter &writer) { writer.enabled_.store(true, std::memory_order_release); }

    static std::size_t queued(const RecoveryWriter &writer)
    {
        return writer.queue_size_.load(std::memory_order_relaxed);
    }
};

}  // namespace spark

namespace {

std::filesystem::path makeTempDir()
{
    auto base = std::filesystem::temp_directory_path() / "spark_recovery_writer_queue_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base;
}

void testWriterQueueDrop()
{
    auto dir = makeTempDir();
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

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writer.stop();

    assert(writer.droppedRecords() > 0);
    assert(writer.writtenRecords() <= cfg.queue_capacity);
    std::cout << "testWriterQueueDrop: PASS (dropped=" << writer.droppedRecords()
              << ", written=" << writer.writtenRecords() << ")\n";
}

void testConcurrentQueueCapacity()
{
    constexpr std::size_t capacity = 7;
    constexpr int producer_count = 8;
    constexpr int attempts_per_producer = 1000;
    constexpr std::uint64_t total_attempts =
        static_cast<std::uint64_t>(producer_count) * static_cast<std::uint64_t>(attempts_per_producer);

    spark::RecoveryWriter::Config cfg;
    cfg.directory = makeTempDir() / "disabled-drain";
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
    writer.stop();
    std::cout << "testConcurrentQueueCapacity: PASS (accepted=" << accepted << ", dropped=" << dropped << ")\n";
}

}  // namespace

int main()
{
    testWriterQueueDrop();
    testConcurrentQueueCapacity();
    std::cout << "All recovery writer queue tests passed.\n";
    return 0;
}
