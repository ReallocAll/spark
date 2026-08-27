#pragma once

#include <string>
#include <string_view>

#include "core/metadata/gamerule_defaults.h"

namespace spark {

namespace detail {

inline std::string normalizeSemanticMinecraftVersion(std::string_view version)
{
    std::string normalized = normalizeMinecraftVersion(version);
    constexpr std::string_view kLegacyPrefix = "1.";
    if (normalized.starts_with(kLegacyPrefix)) {
        normalized.erase(0, kLegacyPrefix.size());
    }
    return normalized;
}

}  // namespace detail

inline bool shouldExportGameRule(std::string_view name, std::string_view minecraft_version)
{
    if (detail::normalizeGameRuleName(name) != "locatorbar") {
        return true;
    }

    const std::string normalized_version = detail::normalizeSemanticMinecraftVersion(minecraft_version);
    if (normalized_version.empty()) {
        // An unknown version must not cause historical data to disappear.
        return true;
    }

    return detail::compareMinecraftVersions(normalized_version, "26.30") < 0;
}

inline std::string normalizeGameRuleSemanticValue(std::string_view name, std::string_view raw_value)
{
    if (detail::normalizeGameRuleName(name) != "playerwaypoints") {
        return std::string(raw_value);
    }

    // Verified against BDS 1.26.44.3 through Endstone's runtime API:
    // command value `off` reads back as 0 and `everyone` reads back as 1.
    // Export the canonical command spellings users see and can enter.
    if (raw_value == "0") {
        return "off";
    }
    if (raw_value == "1") {
        return "everyone";
    }

    std::string unknown = "Unknown";
    if (!raw_value.empty()) {
        unknown += " (";
        unknown += raw_value;
        unknown += ')';
    }
    return unknown;
}

}  // namespace spark
