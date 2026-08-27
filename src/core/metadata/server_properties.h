#ifndef SPARK_CORE_METADATA_SERVER_PROPERTIES_H
#define SPARK_CORE_METADATA_SERVER_PROPERTIES_H

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace spark {

// Parses server.properties with a strict allowlist. The built-in table contains
// known-safe BDS diagnostics; administrators may explicitly append additional
// keys they have reviewed. Known-sensitive keys are never returned.
std::map<std::string, std::string> parseServerProperties(
    const std::filesystem::path &file, const std::vector<std::string> &additional_safe_keys = {});

// Serializes properties as a JSON object string matching upstream spark's server_configurations.
std::string serverPropertiesToJsonString(const std::map<std::string, std::string> &properties);

}  // namespace spark

#endif  // SPARK_CORE_METADATA_SERVER_PROPERTIES_H
