#include "core/metadata/behavior_packs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace spark {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t KMaxMetadataJsonBytes = 4U * 1024U * 1024U;

struct ActivePackReference {
    std::string id;
    std::string version;
};

struct BehaviorPackCandidate {
    std::string id;
    std::string version;
    DataPackInfo info;
};

std::string trim(std::string value)
{
    const auto is_space = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string normalizeUuid(std::string value)
{
    value = trim(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::optional<std::string> readTextFile(const std::filesystem::path &path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > KMaxMetadataJsonBytes) {
        return std::nullopt;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        return std::nullopt;
    }
    return buffer.str();
}

std::optional<Json> readJsonFile(const std::filesystem::path &path)
{
    const auto text = readTextFile(path);
    if (!text.has_value()) {
        return std::nullopt;
    }
    Json json = Json::parse(*text, nullptr, false, true);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return json;
}

std::optional<std::string> canonicalVersion(const Json &value)
{
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (!value.is_array() || value.empty()) {
        return std::nullopt;
    }

    std::string result;
    for (const Json &part : value) {
        if (!part.is_number_integer()) {
            return std::nullopt;
        }
        const auto number = part.get<long long>();
        if (number < 0) {
            return std::nullopt;
        }
        if (!result.empty()) {
            result.push_back('.');
        }
        result += std::to_string(number);
    }
    return result;
}

std::vector<ActivePackReference> readActivePackReferences(const std::filesystem::path &world_root)
{
    const auto json = readJsonFile(world_root / "world_behavior_packs.json");
    if (!json.has_value() || !json->is_array()) {
        return {};
    }

    std::vector<ActivePackReference> references;
    references.reserve(json->size());
    for (const Json &entry : *json) {
        if (!entry.is_object()) {
            continue;
        }
        const auto id_it = entry.find("pack_id");
        if (id_it == entry.end() || !id_it->is_string()) {
            continue;
        }
        std::string id = normalizeUuid(id_it->get<std::string>());
        if (id.empty()) {
            continue;
        }

        std::string version;
        const auto version_it = entry.find("version");
        if (version_it != entry.end()) {
            const auto parsed = canonicalVersion(*version_it);
            if (!parsed.has_value()) {
                continue;
            }
            version = *parsed;
        }
        references.push_back({.id = std::move(id), .version = std::move(version)});
    }
    return references;
}

bool isBehaviorModuleType(std::string type)
{
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return type == "data" || type == "script";
}

std::optional<BehaviorPackCandidate> parseBehaviorManifest(const std::filesystem::path &pack_root,
                                                           std::string_view source)
{
    const auto json = readJsonFile(pack_root / "manifest.json");
    if (!json.has_value() || !json->is_object()) {
        return std::nullopt;
    }

    const auto header_it = json->find("header");
    const auto modules_it = json->find("modules");
    if (header_it == json->end() || !header_it->is_object() || modules_it == json->end() || !modules_it->is_array()) {
        return std::nullopt;
    }

    bool behavior_module = false;
    for (const Json &module : *modules_it) {
        if (!module.is_object()) {
            continue;
        }
        const auto type_it = module.find("type");
        if (type_it != module.end() && type_it->is_string() && isBehaviorModuleType(type_it->get<std::string>())) {
            behavior_module = true;
            break;
        }
    }
    if (!behavior_module) {
        return std::nullopt;
    }

    const auto uuid_it = header_it->find("uuid");
    const auto version_it = header_it->find("version");
    if (uuid_it == header_it->end() || !uuid_it->is_string() || version_it == header_it->end()) {
        return std::nullopt;
    }
    std::string id = normalizeUuid(uuid_it->get<std::string>());
    const auto version = canonicalVersion(*version_it);
    if (id.empty() || !version.has_value()) {
        return std::nullopt;
    }

    std::string name = pack_root.filename().string();
    if (const auto name_it = header_it->find("name");
        name_it != header_it->end() && name_it->is_string() && !name_it->get_ref<const std::string &>().empty()) {
        name = name_it->get<std::string>();
    }

    std::string description;
    if (const auto description_it = header_it->find("description");
        description_it != header_it->end() && description_it->is_string()) {
        description = description_it->get<std::string>();
    }

    return BehaviorPackCandidate{.id = std::move(id),
                                 .version = *version,
                                 .info = {.name = std::move(name),
                                          .description = std::move(description),
                                          .source = std::string(source),
                                          .builtin = false}};
}

void scanBehaviorPackDirectory(const std::filesystem::path &root, std::string_view source,
                               std::vector<BehaviorPackCandidate> &candidates)
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
        return;
    }

    std::filesystem::directory_iterator iterator(root, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry &entry = *iterator;
        std::error_code entry_error;
        if (entry.is_directory(entry_error) && !entry_error) {
            if (auto candidate = parseBehaviorManifest(entry.path(), source); candidate.has_value()) {
                candidates.push_back(std::move(*candidate));
            }
        }
        iterator.increment(error);
    }
}

bool isSafeWorldDirectoryName(std::string_view value)
{
    if (value.empty()) {
        return false;
    }
    const std::filesystem::path path{std::string(value)};
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    return path == path.filename() && path.filename() != "." && path.filename() != "..";
}

std::optional<std::string> levelNameFromServerProperties(const std::filesystem::path &server_root)
{
    const auto text = readTextFile(server_root / "server.properties");
    if (!text.has_value()) {
        return std::nullopt;
    }

    std::istringstream stream(*text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim(std::move(line));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos || trim(line.substr(0, separator)) != "level-name") {
            continue;
        }
        std::string value = trim(line.substr(separator + 1));
        if (isSafeWorldDirectoryName(value)) {
            return value;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> resolveWorldRoot(const std::filesystem::path &server_root,
                                                      std::string_view level_name_hint)
{
    std::vector<std::string> names;
    if (const auto configured = levelNameFromServerProperties(server_root); configured.has_value()) {
        names.push_back(*configured);
    }
    if (isSafeWorldDirectoryName(level_name_hint) &&
        std::find(names.begin(), names.end(), level_name_hint) == names.end()) {
        names.emplace_back(level_name_hint);
    }
    if (names.empty()) {
        return std::nullopt;
    }

    for (const std::string &name : names) {
        const auto world_root = server_root / "worlds" / name;
        std::error_code error;
        if (std::filesystem::is_regular_file(world_root / "world_behavior_packs.json", error) && !error) {
            return world_root;
        }
    }
    return server_root / "worlds" / names.front();
}

}  // namespace

std::vector<DataPackInfo> discoverActiveBehaviorPacks(const std::filesystem::path &server_root,
                                                      std::string_view level_name_hint)
{
    const auto world_root = resolveWorldRoot(server_root, level_name_hint);
    if (!world_root.has_value()) {
        return {};
    }
    const auto references = readActivePackReferences(*world_root);
    if (references.empty()) {
        return {};
    }

    std::vector<BehaviorPackCandidate> candidates;
    scanBehaviorPackDirectory(*world_root / "behavior_packs", "world", candidates);
    scanBehaviorPackDirectory(server_root / "behavior_packs", "server", candidates);
    scanBehaviorPackDirectory(server_root / "development_behavior_packs", "server", candidates);

    std::vector<DataPackInfo> result;
    std::set<std::pair<std::string, std::string>> emitted;
    for (const ActivePackReference &reference : references) {
        const auto candidate =
            std::find_if(candidates.begin(), candidates.end(), [&](const BehaviorPackCandidate &entry) {
                return entry.id == reference.id && (reference.version.empty() || entry.version == reference.version);
            });
        if (candidate == candidates.end()) {
            continue;
        }
        const auto key = std::pair{candidate->id, candidate->version};
        if (!emitted.insert(key).second) {
            continue;
        }
        result.push_back(candidate->info);
    }
    return result;
}

}  // namespace spark
