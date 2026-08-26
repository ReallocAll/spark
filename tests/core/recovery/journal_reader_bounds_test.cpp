#include <zlib.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

#include "core/recovery/journal_format.h"
#include "core/recovery/journal_reader.h"

using namespace spark;  // NOLINT(google-build-using-namespace)

namespace {

std::filesystem::path makeTempDir(const char *name)
{
    const auto directory = std::filesystem::temp_directory_path() / "spark_journal_reader_bounds" / name;
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

void writeBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    assert(file != nullptr);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
}

struct RecordSpec {
    RecordType type;
    std::uint32_t sequence;
    JournalBuffer payload;
};

std::vector<std::uint8_t> makeSegment(std::uint64_t session_id, std::uint32_t segment_number,
                                      const std::vector<RecordSpec> &records)
{
    auto bytes = serializeFileHeader(session_id, session_id, segment_number);
    for (const auto &record : records) {
        auto serialized = serializeRecord(record.type, record.sequence, record.payload);
        bytes.insert(bytes.end(), serialized.begin(), serialized.end());
    }
    return bytes;
}

void writeSegment(const std::filesystem::path &path, std::uint64_t session_id, std::uint32_t segment_number,
                  const std::vector<RecordSpec> &records)
{
    writeBytes(path, makeSegment(session_id, segment_number, records));
}

JournalBuffer tickPayload()
{
    return buildTickEventPayload(1, 5.0);
}

void testFinalTailSalvage()
{
    const auto directory = makeTempDir("final-tail");
    auto bytes = makeSegment(100, 0, {{.type = RecordType::TickEvent, .sequence = 0, .payload = tickPayload()}});
    bytes.insert(bytes.end(), {static_cast<std::uint8_t>(RecordType::TickEvent), 0, 1, 0, 0, 0});
    writeBytes(directory / "segment-0.jnl", bytes);

    const auto result = JournalReader::readSession(directory);
    assert(result.valid);
    assert(!result.fatal_error);
    assert(result.tail_truncated);
    assert(result.truncated_records == 1);
    assert(result.record_count == 1);
    assert(result.records.size() == 1);
}

void testFinalCrcSalvageAndMiddleCrcFatal()
{
    const auto final_directory = makeTempDir("final-crc");
    auto corrupt = serializeRecord(RecordType::TickEvent, 0, tickPayload());
    corrupt[kRecordHeaderSize] ^= 0xff;
    auto final_bytes = serializeFileHeader(101, 101, 0);
    final_bytes.insert(final_bytes.end(), corrupt.begin(), corrupt.end());
    writeBytes(final_directory / "segment-0.jnl", final_bytes);

    auto final_result = JournalReader::readSession(final_directory);
    assert(final_result.valid);
    assert(final_result.tail_corrupt);
    assert(final_result.corrupt_records == 1);
    assert(final_result.records.empty());

    const auto middle_directory = makeTempDir("middle-crc");
    writeBytes(middle_directory / "segment-0.jnl", final_bytes);
    writeSegment(middle_directory / "segment-1.jnl", 101, 1,
                 {{.type = RecordType::TickEvent, .sequence = 1, .payload = tickPayload()}});
    const auto middle_result = JournalReader::readSession(middle_directory);
    assert(!middle_result.valid);
    assert(middle_result.fatal_error);
    assert(middle_result.malformed_segment);
}

void testFinalCrcWithLaterBytesIsFatal()
{
    const auto directory = makeTempDir("final-crc-with-later-bytes");
    auto corrupt = serializeRecord(RecordType::TickEvent, 0, tickPayload());
    corrupt[kRecordHeaderSize] ^= 0xff;
    auto bytes = serializeFileHeader(102, 102, 0);
    bytes.insert(bytes.end(), corrupt.begin(), corrupt.end());
    auto valid = serializeRecord(RecordType::TickEvent, 1, tickPayload());
    bytes.insert(bytes.end(), valid.begin(), valid.end());
    writeBytes(directory / "segment-0.jnl", bytes);

    const auto result = JournalReader::readSession(directory);
    assert(!result.valid);
    assert(result.fatal_error);
    assert(result.corrupt_records == 1);
}

void testNonFinalTruncationAndMissingMiddle()
{
    const auto trunc_directory = makeTempDir("middle-truncation");
    auto truncated = makeSegment(103, 0, {{.type = RecordType::TickEvent, .sequence = 0, .payload = tickPayload()}});
    truncated.insert(truncated.end(), {static_cast<std::uint8_t>(RecordType::TickEvent), 0, 1, 0, 0, 0});
    writeBytes(trunc_directory / "segment-0.jnl", truncated);
    writeSegment(trunc_directory / "segment-1.jnl", 103, 1,
                 {{.type = RecordType::TickEvent, .sequence = 1, .payload = tickPayload()}});
    const auto trunc_result = JournalReader::readSession(trunc_directory);
    assert(!trunc_result.valid);
    assert(trunc_result.fatal_error);
    assert(trunc_result.truncated_records == 1);

    const auto gap_directory = makeTempDir("missing-middle");
    writeSegment(gap_directory / "segment-0.jnl", 104, 0,
                 {{.type = RecordType::TickEvent, .sequence = 0, .payload = tickPayload()}});
    writeSegment(gap_directory / "segment-2.jnl", 104, 2,
                 {{.type = RecordType::TickEvent, .sequence = 2, .payload = tickPayload()}});
    const auto gap_result = JournalReader::readSession(gap_directory);
    assert(!gap_result.valid);
    assert(gap_result.gap_detected);
    assert(gap_result.fatal_error);
}

void testDuplicateSegmentsAndSequences()
{
    const auto duplicate_segment_directory = makeTempDir("duplicate-segment");
    const auto bytes = makeSegment(105, 0, {{.type = RecordType::TickEvent, .sequence = 0, .payload = tickPayload()}});
    writeBytes(duplicate_segment_directory / "segment-0.jnl", bytes);
    writeBytes(duplicate_segment_directory / "segment-00.jnl", bytes);
    const auto duplicate_segment_result = JournalReader::readSession(duplicate_segment_directory);
    assert(!duplicate_segment_result.valid);
    assert(duplicate_segment_result.fatal_error);

    const auto duplicate_sequence_directory = makeTempDir("duplicate-sequence");
    writeSegment(duplicate_sequence_directory / "segment-0.jnl", 106, 0,
                 {{.type = RecordType::TickEvent, .sequence = 7, .payload = tickPayload()},
                  {.type = RecordType::TickEvent, .sequence = 7, .payload = tickPayload()}});
    const auto duplicate_sequence_result = JournalReader::readSession(duplicate_sequence_directory);
    assert(!duplicate_sequence_result.valid);
    assert(duplicate_sequence_result.duplicate_sequences);
    assert(duplicate_sequence_result.fatal_error);
}

void testHeadAndSnapshotBounds()
{
    const auto head_directory = makeTempDir("head-truncated");
    writeSegment(head_directory / "segment-3.jnl", 107, 3,
                 {{.type = RecordType::TickEvent, .sequence = 0, .payload = tickPayload()}});
    const auto head_result = JournalReader::readSession(head_directory);
    assert(head_result.valid);
    assert(head_result.head_truncated);
    assert(head_result.first_segment_number == 3);

    const auto snapshot_directory = makeTempDir("snapshot-bounds");
    auto snapshot = serializeMetadataSnapshot(108, 0, {}, {}, {});
    const std::uint32_t malicious_count = std::numeric_limits<std::uint32_t>::max();
    std::memcpy(snapshot.data() + kSnapshotHeaderSize + sizeof(std::uint16_t), &malicious_count,
                sizeof(malicious_count));
    const auto payload_size = static_cast<std::uint32_t>(snapshot.size() - kSnapshotHeaderSize);
    const auto crc = static_cast<std::uint32_t>(crc32(0L, snapshot.data() + kSnapshotHeaderSize, payload_size));
    constexpr std::size_t crc_offset = kSnapshotHeaderSize - sizeof(std::uint32_t);
    std::memcpy(snapshot.data() + crc_offset, &crc, sizeof(crc));
    writeBytes(snapshot_directory / "metadata.snapshot", snapshot);
    assert(!JournalReader::readMetadataSnapshot(snapshot_directory).valid);

    snapshot = serializeMetadataSnapshot(108, 0, {}, {}, {});
    snapshot.push_back(0xff);
    writeBytes(snapshot_directory / "metadata.snapshot", snapshot);
    assert(!JournalReader::readMetadataSnapshot(snapshot_directory).valid);
}

void testLimitsAndUnreadableInput()
{
    JournalReaderLimits limits;
    limits.segment_bytes = 128;
    limits.total_bytes = 256;
    limits.segments = 4;
    limits.records = 3;

    const auto segment_directory = makeTempDir("segment-limit");
    writeBytes(segment_directory / "segment-0.jnl", serializeFileHeader(109, 109, 0));
    std::filesystem::resize_file(segment_directory / "segment-0.jnl", limits.segment_bytes + 1);
    const auto segment_result = JournalReader::readSession(segment_directory, limits);
    assert(!segment_result.valid);
    assert(segment_result.limit_exceeded);

    const auto aggregate_directory = makeTempDir("aggregate-limit");
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto path = aggregate_directory / ("segment-" + std::to_string(i) + ".jnl");
        writeBytes(path, serializeFileHeader(110, 110, i));
        std::filesystem::resize_file(path, 96);
    }
    const auto aggregate_result = JournalReader::readSession(aggregate_directory, limits);
    assert(!aggregate_result.valid);
    assert(aggregate_result.limit_exceeded);

    const auto count_directory = makeTempDir("segment-count-limit");
    for (std::uint32_t i = 0; i <= limits.segments; ++i) {
        writeBytes(count_directory / ("segment-" + std::to_string(i) + ".jnl"), serializeFileHeader(111, 111, i));
    }
    const auto count_result = JournalReader::readSession(count_directory, limits);
    assert(!count_result.valid);
    assert(count_result.limit_exceeded);

    const auto record_directory = makeTempDir("record-count-limit");
    auto record_bytes = serializeFileHeader(112, 112, 0);
    const auto empty_payload = JournalBuffer{};
    const auto empty_record = serializeRecord(RecordType::TickEvent, 0, empty_payload);
    for (std::uint32_t i = 0; i <= limits.records; ++i) {
        auto record = empty_record;
        std::memcpy(record.data() + 2, &i, sizeof(i));
        record_bytes.insert(record_bytes.end(), record.begin(), record.end());
    }
    writeBytes(record_directory / "segment-0.jnl", record_bytes);
    const auto record_result = JournalReader::readSession(record_directory, limits);
    assert(!record_result.valid);
    assert(record_result.limit_exceeded);
    assert(record_result.record_count == limits.records);

    JournalReadResult unreadable;
    assert(!JournalReader::readSegment(record_directory / "missing.jnl", unreadable));
    assert(unreadable.unreadable_segment);
    assert(unreadable.fatal_error);
}

}  // namespace

int main()
{
    testFinalTailSalvage();
    testFinalCrcSalvageAndMiddleCrcFatal();
    testFinalCrcWithLaterBytesIsFatal();
    testNonFinalTruncationAndMissingMiddle();
    testDuplicateSegmentsAndSequences();
    testHeadAndSnapshotBounds();
    testLimitsAndUnreadableInput();
    return 0;
}
