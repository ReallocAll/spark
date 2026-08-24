#ifndef ENDSTONE_SPARK_CORE_UTIL_STATE_FILE_H
#define ENDSTONE_SPARK_CORE_UTIL_STATE_FILE_H

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace spark {

bool readStateFile(const std::filesystem::path &path, std::size_t max_bytes, std::string &out, std::string &error);

bool writeStateFileAtomically(const std::filesystem::path &path, std::string_view data, std::string &error);

}  // namespace spark

#endif  // ENDSTONE_SPARK_CORE_UTIL_STATE_FILE_H
