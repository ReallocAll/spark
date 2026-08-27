#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "core/recovery/journal_format.h"
#include "core/recovery/journal_reader.h"
#include "journal_test_cases.h"
#include "journal_test_support.h"
#include "native/sampler/types.h"

using namespace spark;                // NOLINT(google-build-using-namespace)
using namespace spark::journal_test;  // NOLINT(google-build-using-namespace)

void testFileHeaderMagic()
{
    auto header = serializeFileHeader(42, 1000, 0);
    assert(header.size() == kFileHeaderSize);
    assert(std::memcmp(header.data(), kJournalMagic, 8) == 0);

    std::uint16_t version;
    std::memcpy(&version, header.data() + 8, 2);
    assert(version == kJournalVersion);
    std::cout << "testFileHeaderMagic: PASS\n";
}

void testSupportedJournalVersions()
{
    const auto dir = makeTempDir();
    const auto path = dir / "segment-0.jnl";
    for (const std::uint16_t version : {kLegacyJournalVersion, kPreviousJournalVersion, kJournalVersion}) {
        const auto header = serializeFileHeader(9, 10, 0, version);
        std::FILE *file = std::fopen(path.string().c_str(), "wb");
        assert(file);
        assert(std::fwrite(header.data(), 1, header.size(), file) == header.size());
        std::fclose(file);
        JournalReadResult result;
        assert(JournalReader::readSegment(path, result));
        assert(result.valid);
        assert(result.version == version);
    }
    std::cout << "testSupportedJournalVersions: PASS\n";
}

void testRecordSerialization()
{
    JournalBuffer payload = buildTickEventPayload(7, 42.5);
    auto record = serializeRecord(RecordType::TickEvent, 1, payload);

    // Record header: 1 (type) + 1 (reserved) + 4 (seq) + 4 (len) + 4 (crc) = 14
    assert(record.size() == kRecordHeaderSize + payload.size());
    assert(record[0] == static_cast<std::uint8_t>(RecordType::TickEvent));
    std::cout << "testRecordSerialization: PASS\n";
}

void testModuleDefRoundTrip()
{
    JournalBuffer payload = buildModuleDefPayload(5, "/usr/lib/libc.so");
    auto record = serializeRecord(RecordType::ModuleDef, 0, payload);

    // Parse it back.
    JournalReadResult result;
    // Simulate a file: header + record.
    auto header = serializeFileHeader(1, 0, 0);
    std::vector<std::uint8_t> file_buf;
    file_buf.insert(file_buf.end(), header.begin(), header.end());
    file_buf.insert(file_buf.end(), record.begin(), record.end());

    // Write to temp file and read.
    auto dir = makeTempDir();
    auto path = dir / "segment-0.jnl";
    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    assert(f);
    std::fwrite(file_buf.data(), 1, file_buf.size(), f);
    std::fclose(f);

    JournalReadResult result2;
    bool ok = JournalReader::readSegment(path, result2);
    assert(ok);
    assert(result2.valid);
    assert(result2.records.size() == 1);
    assert(result2.records[0].type == RecordType::ModuleDef);

    std::uint32_t module_id;
    std::string module_path;
    assert(result2.records[0].asModuleDef(module_id, module_path));
    assert(module_id == 5);
    assert(module_path == "/usr/lib/libc.so");
    std::cout << "testModuleDefRoundTrip: PASS\n";
}

void testSampleRoundTrip()
{
    Sample sample;
    sample.thread_id = 12345;
    sample.tick_id = 67;
    sample.window = 3;
    sample.weight = 4000;
    sample.thread_name = "Server thread";
    sample.frames.push_back({.module = 1, .rva = 0x1000, .raw_address = 0xABCD});
    sample.frames.push_back({.module = 2, .rva = 0x2000, .raw_address = 0xDCBA});

    JournalBuffer payload = buildSamplePayload(sample);
    auto record = serializeRecord(RecordType::Sample, 10, payload);
    auto header = serializeFileHeader(1, 0, 0);

    std::vector<std::uint8_t> file_buf;
    file_buf.insert(file_buf.end(), header.begin(), header.end());
    file_buf.insert(file_buf.end(), record.begin(), record.end());

    auto dir = makeTempDir();
    auto path = dir / "segment-0.jnl";
    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    assert(f);
    std::fwrite(file_buf.data(), 1, file_buf.size(), f);
    std::fclose(f);

    JournalReadResult result;
    bool ok = JournalReader::readSegment(path, result);
    assert(ok);
    assert(result.records.size() == 1);
    assert(result.records[0].type == RecordType::Sample);

    std::uint64_t thread_id, tick_id, weight;
    std::int32_t window;
    std::vector<FrameKey> frames;
    assert(result.records[0].asSample(thread_id, tick_id, window, weight, frames));
    assert(thread_id == 12345);
    assert(tick_id == 67);
    assert(window == 3);
    assert(weight == 4000);
    assert(frames.size() == 2);
    assert(frames[0].module == 1);
    assert(frames[0].rva == 0x1000);
    assert(frames[1].module == 2);
    assert(frames[1].rva == 0x2000);
    std::cout << "testSampleRoundTrip: PASS\n";
}

void testSessionConfigRoundTrip()
{
    std::vector<std::string> patterns = {"Server thread", "Worker-*"};
    JournalBuffer payload =
        buildSessionConfigPayload(8000, 50, true, false, true, 2, 1, true, "PlayerOne", true, "test profile", patterns,
                                  0, "123e4567-e89b-12d3-a456-426614174000");
    auto record = serializeRecord(RecordType::SessionConfig, 0, payload);

    JournalRecord rec;
    rec.type = RecordType::SessionConfig;
    rec.sequence = 0;
    rec.payload.assign(record.begin() + kRecordHeaderSize, record.end());

    SessionConfig sc;
    assert(rec.asSessionConfig(sc));
    assert(sc.present);
    assert(sc.interval_us == 8000);
    assert(sc.only_ticks_over_ms == 50);
    assert(sc.all_threads);
    assert(!sc.regex_threads);
    assert(sc.ignore_sleeping);
    assert(sc.thread_grouper == 2);
    assert(sc.profile_type == 1);
    assert(sc.live_only);
    assert(sc.creator_name == "PlayerOne");
    assert(sc.creator_is_player);
    assert(sc.creator_unique_id == "123e4567-e89b-12d3-a456-426614174000");
    assert(sc.comment == "test profile");
    assert(sc.thread_patterns.size() == 2);
    assert(sc.thread_patterns[0] == "Server thread");
    assert(sc.thread_patterns[1] == "Worker-*");
    assert(sc.has_window_adjustment);
    assert(sc.window_adjustment_ms == 0);

    JournalBuffer previous_payload =
        buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 17);
    auto previous_bytes = previous_payload.take();
    assert(previous_bytes.size() >= 2);
    previous_bytes.resize(previous_bytes.size() - 2);  // v3 had no UUID string suffix.
    JournalRecord previous_record{.type = RecordType::SessionConfig, .sequence = 1, .payload = previous_bytes};
    SessionConfig previous_config;
    assert(previous_record.asSessionConfig(previous_config));
    assert(previous_config.has_window_adjustment);
    assert(previous_config.window_adjustment_ms == 17);
    assert(previous_config.creator_unique_id.empty());

    assert(previous_bytes.size() >= sizeof(std::int32_t));
    previous_bytes.resize(previous_bytes.size() - sizeof(std::int32_t));  // v2 had no window adjustment.
    JournalRecord legacy_record{.type = RecordType::SessionConfig, .sequence = 2, .payload = previous_bytes};
    SessionConfig legacy_config;
    assert(legacy_record.asSessionConfig(legacy_config));
    assert(!legacy_config.has_window_adjustment);
    assert(legacy_config.creator_unique_id.empty());

    std::cout << "testSessionConfigRoundTrip: PASS\n";
}

void testSessionConfigTrailingBytesRejected()
{
    auto extended = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0);
    JournalBuffer malformed;
    malformed.bytes(extended.data(), extended.size() - 3);
    JournalRecord record{.type = RecordType::SessionConfig, .sequence = 0};
    record.payload = malformed.take();
    SessionConfig config;
    assert(!record.asSessionConfig(config));
    std::cout << "testSessionConfigTrailingBytesRejected: PASS\n";
}
