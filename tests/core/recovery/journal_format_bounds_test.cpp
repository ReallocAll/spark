#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "core/recovery/journal_format.h"

using namespace spark;  // NOLINT(google-build-using-namespace)

namespace {

constexpr std::size_t KMaxU16 = std::numeric_limits<std::uint16_t>::max();

template <typename T>
T readAt(const std::uint8_t *data, std::size_t offset)
{
    T value{};
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

void testOversizedString()
{
    const std::string input(KMaxU16 + 1, 'x');
    JournalBuffer buffer;
    buffer.str(input);

    assert(buffer.size() == sizeof(std::uint16_t) + KMaxU16);
    assert(readAt<std::uint16_t>(buffer.data(), 0) == std::numeric_limits<std::uint16_t>::max());
    assert(std::memcmp(buffer.data() + sizeof(std::uint16_t), input.data(), KMaxU16) == 0);

    const auto record = serializeRecord(RecordType::ModuleDef, 1, buffer);
    assert(record.size() == kRecordHeaderSize + buffer.size());
}

void testSampleFrameCount()
{
    Sample sample;
    sample.frames.resize(KMaxU16 + 1);
    for (std::size_t i = 0; i < sample.frames.size(); ++i) {
        sample.frames[i].module = static_cast<std::uint32_t>(i);
        sample.frames[i].rva = i;
    }

    const auto payload = buildSamplePayload(sample);
    constexpr std::size_t k_fixed_payload_size =
        sizeof(std::uint64_t) * 3 + sizeof(std::int32_t) + sizeof(std::uint16_t);
    constexpr std::size_t k_frame_size = sizeof(std::uint32_t) + sizeof(std::uint64_t);
    assert(readAt<std::uint16_t>(payload.data(), 28) == std::numeric_limits<std::uint16_t>::max());
    assert(payload.size() == k_fixed_payload_size + KMaxU16 * k_frame_size);

    const auto record = serializeRecord(RecordType::Sample, 1, payload);
    assert(record.size() == kRecordHeaderSize + payload.size());
}

void testSessionPatternCount()
{
    const std::vector<std::string> patterns(KMaxU16 + 1, "p");
    const auto payload = buildSessionConfigPayload(1, 0, false, false, false, 0, 0, false, "", false, "", patterns, 0);

    constexpr std::size_t k_pattern_count_offset = 4 + 4 + 6 + 2 + 1 + 2;
    constexpr std::size_t k_pattern_size = sizeof(std::uint16_t) + 1;
    assert(readAt<std::uint16_t>(payload.data(), k_pattern_count_offset) == std::numeric_limits<std::uint16_t>::max());
    assert(payload.size() == k_pattern_count_offset + sizeof(std::uint16_t) + KMaxU16 * k_pattern_size +
                                 sizeof(std::int32_t) + sizeof(std::uint16_t));

    const auto record = serializeRecord(RecordType::SessionConfig, 1, payload);
    assert(record.size() == kRecordHeaderSize + payload.size());
}

void testSnapshotSessionConfigLength()
{
    const std::vector<std::uint8_t> session_config(KMaxU16 + 1, 0xab);
    const auto snapshot = serializeMetadataSnapshot(1, 2, session_config, {}, {});
    const std::size_t session_config_offset = kSnapshotHeaderSize + sizeof(std::uint16_t);
    const std::size_t module_count_offset = session_config_offset + KMaxU16;
    const std::size_t thread_count_offset = module_count_offset + sizeof(std::uint32_t);

    assert(readAt<std::uint16_t>(snapshot.data(), kSnapshotHeaderSize) == std::numeric_limits<std::uint16_t>::max());
    assert(std::memcmp(snapshot.data() + session_config_offset, session_config.data(), KMaxU16) == 0);
    assert(readAt<std::uint32_t>(snapshot.data(), module_count_offset) == 0);
    assert(readAt<std::uint32_t>(snapshot.data(), thread_count_offset) == 0);
    assert(readAt<std::uint32_t>(snapshot.data(), 28) == snapshot.size() - kSnapshotHeaderSize);
    assert(snapshot.size() == kSnapshotHeaderSize + sizeof(std::uint16_t) + KMaxU16 + sizeof(std::uint32_t) * 2);
}

}  // namespace

int main()
{
    testOversizedString();
    testSampleFrameCount();
    testSessionPatternCount();
    testSnapshotSessionConfigLength();
    return 0;
}
