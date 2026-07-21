#ifndef ENDSTONE_SPARK_PROFILE_FILE_H
#define ENDSTONE_SPARK_PROFILE_FILE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace spark {

struct ProfileFileResult {
    bool ok = false;
    std::filesystem::path path;
    std::string error;
};

// Atomically writes a raw SamplerData protobuf payload to a unique .sparkprofile
// file in `folder`. Existing profiles are never overwritten.
ProfileFileResult saveProfileToDirectory(const std::filesystem::path &folder,
                                         std::string_view profile,
                                         std::int64_t timestamp_ms);

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROFILE_FILE_H
