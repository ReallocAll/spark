#ifndef SPARK_TESTS_CORE_RECOVERY_JOURNAL_TEST_SUPPORT_H
#define SPARK_TESTS_CORE_RECOVERY_JOURNAL_TEST_SUPPORT_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/recovery/journal_format.h"

namespace spark::journal_test {

std::filesystem::path makeTempDir();

struct RecordSpec {
    RecordType type;
    std::uint32_t sequence;
    JournalBuffer payload;
};

void writeSegment(const std::filesystem::path &path, std::uint64_t session_id, std::uint32_t segment_number,
                  std::uint32_t sequence, RecordType type, const JournalBuffer &payload,
                  std::uint16_t version = kJournalVersion);

JournalBuffer buildLegacySessionConfigPayload();

void writeSegmentMulti(const std::filesystem::path &path, std::uint64_t session_id, std::uint32_t segment_number,
                       const std::vector<RecordSpec> &records, std::uint16_t version = kJournalVersion);

void writeBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes);

}  // namespace spark::journal_test

#endif  // SPARK_TESTS_CORE_RECOVERY_JOURNAL_TEST_SUPPORT_H
