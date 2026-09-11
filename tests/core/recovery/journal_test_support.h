#ifndef SPARK_TESTS_CORE_RECOVERY_JOURNAL_TEST_SUPPORT_H
#define SPARK_TESTS_CORE_RECOVERY_JOURNAL_TEST_SUPPORT_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/profiler/profile_mode.h"
#include "core/recovery/journal_format.h"
#include "proto/proto_reader.h"

namespace spark::journal_test {

inline std::vector<double> groupingWeights(std::string_view bytes)
{
    if (bytes.size() % sizeof(double) != 0) {
        return {};
    }
    std::vector<double> values(bytes.size() / sizeof(double));
    if (!bytes.empty()) {
        std::memcpy(values.data(), bytes.data(), bytes.size());
    }
    return values;
}

inline bool verifyGroupedProfile(std::string_view profile, ThreadGrouperMode mode, bool allocation)
{
    const double scale = allocation ? 1.0 : 0.001;
    std::size_t thread_count = 0;
    std::vector<std::int32_t> windows;
    ProtoReader data(profile);
    int field = 0;
    int wire = 0;
    while (data.nextField(field, wire)) {
        if (field == 6 && wire == 2) {
            auto packed = data.readMessage();
            while (!packed.eof()) {
                windows.push_back(packed.readInt32());
            }
            if (!packed.valid()) {
                return false;
            }
        }
        else if (field == 2 && wire == 2) {
            auto thread = data.readMessage();
            std::string name;
            std::vector<double> weights;
            std::map<std::uint64_t, std::vector<double>> leaves;
            std::vector<std::int32_t> roots;
            std::size_t nodes = 0;
            while (thread.nextField(field, wire)) {
                if (field == 1 && wire == 2) {
                    name = thread.readString();
                }
                else if (field == 4 && wire == 2) {
                    weights = groupingWeights(thread.readString());
                }
                else if (field == 5 && wire == 2) {
                    auto refs = thread.readMessage();
                    while (!refs.eof()) {
                        roots.push_back(refs.readInt32());
                    }
                    if (!refs.valid()) {
                        return false;
                    }
                }
                else if (field == 3 && wire == 2) {
                    auto node = thread.readMessage();
                    std::uint64_t rva = 0;
                    std::vector<double> times;
                    while (node.nextField(field, wire)) {
                        if (field == 1001 && wire == 0) {
                            rva = node.readVarint();
                        }
                        else if (field == 8 && wire == 2) {
                            times = groupingWeights(node.readString());
                        }
                        else if (field == 9 && wire == 2) {
                            if (!node.readString().empty()) {
                                return false;
                            }
                        }
                        else {
                            node.skip(wire);
                        }
                    }
                    if (!node.valid() || !leaves.emplace(rva, times).second) {
                        return false;
                    }
                    ++nodes;
                }
                else {
                    thread.skip(wire);
                }
            }
            const bool separate = mode == ThreadGrouperMode::ByName;
            std::string expected_name = "Worker";
            if (!separate) {
                expected_name = mode == ThreadGrouperMode::ByPool ? "Worker (x2)" : "All (x2)";
            }
            std::map<std::uint64_t, std::vector<double>> expected_leaves;
            if (!separate || thread_count == 0) {
                expected_leaves.emplace(0x1110, std::vector<double>{2000 * scale, 3000 * scale});
            }
            if (!separate || thread_count == 1) {
                expected_leaves.emplace(0x2220, std::vector<double>{5000 * scale, 7000 * scale});
            }
            std::vector<double> expected_weights{7000 * scale, 10000 * scale};
            if (separate) {
                expected_weights = thread_count == 0 ? std::vector<double>{2000 * scale, 3000 * scale}
                                                     : std::vector<double>{5000 * scale, 7000 * scale};
            }
            if (!thread.valid() || name != expected_name || weights != expected_weights || leaves != expected_leaves ||
                nodes != expected_leaves.size() || roots.size() != nodes) {
                return false;
            }
            std::sort(roots.begin(), roots.end());
            for (std::size_t i = 0; i < roots.size(); ++i) {
                if (roots[i] != static_cast<std::int32_t>(i)) {
                    return false;
                }
            }
            ++thread_count;
        }
        else {
            data.skip(wire);
        }
    }
    return data.valid() && windows == std::vector<std::int32_t>{0, 1} &&
           thread_count == (mode == ThreadGrouperMode::ByName ? 2 : 1);
}

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
