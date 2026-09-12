#ifndef SPARK_CORE_RECOVERY_RECOVERY_PLAYER_H
#define SPARK_CORE_RECOVERY_RECOVERY_PLAYER_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace spark {

struct RecoveredProfile {
    bool valid = false;
    bool resource_limit_exceeded = false;
    std::string serialized_proto;  // uncompressed spark protobuf
    std::int64_t session_start_ms = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t thread_count = 0;
    std::uint64_t tick_count = 0;
    bool has_clean_end = false;
    std::uint64_t corrupt_records = 0;
    std::uint64_t truncated_records = 0;
    std::string error;
};

// Reconstructs a spark profile from crash-recovery journal files.
class RecoveryPlayer {
public:
    static RecoveredProfile replay(const std::filesystem::path &directory);

private:
    friend struct RecoveryPlayerTestAccess;
    static RecoveredProfile replay(const std::filesystem::path &directory, std::size_t remaining_nodes,
                                   std::size_t remaining_time_entries, std::size_t thread_capacity);
};

}  // namespace spark

#endif  // SPARK_CORE_RECOVERY_RECOVERY_PLAYER_H
