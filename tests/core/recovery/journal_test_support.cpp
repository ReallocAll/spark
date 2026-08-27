#include "journal_test_support.h"

#include <cassert>
#include <cstdio>
#include <filesystem>

namespace spark::journal_test {

std::filesystem::path makeTempDir()
{
    auto base = std::filesystem::temp_directory_path() / "spark_journal_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base;
}

void writeSegment(const std::filesystem::path &path, std::uint64_t session_id, std::uint32_t segment_number,
                  std::uint32_t sequence, RecordType type, const JournalBuffer &payload, std::uint16_t version)
{
    auto header = serializeFileHeader(session_id, session_id, segment_number, version);
    auto record = serializeRecord(type, sequence, payload);
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    assert(file);
    assert(std::fwrite(header.data(), 1, header.size(), file) == header.size());
    assert(std::fwrite(record.data(), 1, record.size(), file) == record.size());
    std::fclose(file);
}

JournalBuffer buildLegacySessionConfigPayload()
{
    auto extended = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0);
    JournalBuffer legacy;
    constexpr std::size_t k_v4_suffix_size = sizeof(std::int32_t) + sizeof(std::uint16_t);
    assert(extended.size() >= k_v4_suffix_size);
    legacy.bytes(extended.data(), extended.size() - k_v4_suffix_size);
    return legacy;
}

void writeSegmentMulti(const std::filesystem::path &path, std::uint64_t session_id, std::uint32_t segment_number,
                       const std::vector<RecordSpec> &records, std::uint16_t version)
{
    auto header = serializeFileHeader(session_id, session_id, segment_number, version);
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    assert(file);
    assert(std::fwrite(header.data(), 1, header.size(), file) == header.size());
    for (const auto &rec : records) {
        auto record = serializeRecord(rec.type, rec.sequence, rec.payload);
        assert(std::fwrite(record.data(), 1, record.size(), file) == record.size());
    }
    std::fclose(file);
}

void writeBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
}

}  // namespace spark::journal_test
