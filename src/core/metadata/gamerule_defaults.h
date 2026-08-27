#pragma once

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace spark {

enum class GameRuleValueKind {
    Boolean,
    Integer,
    Enum,
};

struct GameRuleMigration {
    std::string_view old_name;
    std::string_view new_name;
    std::string_view changed_in;
    GameRuleValueKind old_kind;
    GameRuleValueKind new_kind;
};

namespace detail {

inline std::string normalizeGameRuleName(std::string_view name)
{
    constexpr std::string_view kMinecraftNamespace = "minecraft:";
    if (name.size() >= kMinecraftNamespace.size()) {
        bool namespace_match = true;
        for (std::size_t i = 0; i < kMinecraftNamespace.size(); ++i) {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(name[i]))) != kMinecraftNamespace[i]) {
                namespace_match = false;
                break;
            }
        }
        if (namespace_match) {
            name.remove_prefix(kMinecraftNamespace.size());
        }
    }

    std::string normalized;
    normalized.reserve(name.size());
    for (const char ch : name) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

inline std::string normalizeMinecraftVersion(std::string_view version)
{
    while (!version.empty() && (version.front() == 'v' || version.front() == 'V' || version.front() == ' ')) {
        version.remove_prefix(1);
    }

    std::string normalized;
    normalized.reserve(version.size());
    for (const char ch : version) {
        if ((ch >= '0' && ch <= '9') || ch == '.') {
            normalized.push_back(ch);
            continue;
        }
        break;
    }
    while (!normalized.empty() && normalized.back() == '.') {
        normalized.pop_back();
    }
    return normalized;
}

inline int compareMinecraftVersions(std::string_view lhs, std::string_view rhs)
{
    while (!lhs.empty() || !rhs.empty()) {
        unsigned long long lhs_part = 0;
        unsigned long long rhs_part = 0;

        while (!lhs.empty() && lhs.front() != '.') {
            lhs_part = lhs_part * 10ULL + static_cast<unsigned long long>(lhs.front() - '0');
            lhs.remove_prefix(1);
        }
        while (!rhs.empty() && rhs.front() != '.') {
            rhs_part = rhs_part * 10ULL + static_cast<unsigned long long>(rhs.front() - '0');
            rhs.remove_prefix(1);
        }

        if (lhs_part < rhs_part) {
            return -1;
        }
        if (lhs_part > rhs_part) {
            return 1;
        }

        if (!lhs.empty()) {
            lhs.remove_prefix(1);
        }
        if (!rhs.empty()) {
            rhs.remove_prefix(1);
        }
    }
    return 0;
}

inline constexpr std::pair<std::string_view, std::string_view> kCurrentDefaults[] = {
    {"commandblockoutput", "true"},
    {"commandblocksenabled", "true"},
    {"dodaylightcycle", "true"},
    {"doentitydrops", "true"},
    {"dofiretick", "true"},
    {"doimmediaterespawn", "false"},
    {"doinsomnia", "true"},
    {"dolimitedcrafting", "false"},
    {"domobloot", "true"},
    {"domobspawning", "true"},
    {"dotiledrops", "true"},
    {"doweathercycle", "true"},
    {"drowningdamage", "true"},
    {"falldamage", "true"},
    {"firedamage", "true"},
    {"freezedamage", "true"},
    {"functioncommandlimit", "10000"},
    {"keepinventory", "false"},
    {"maxcommandchainlength", "65535"},
    {"mobgriefing", "true"},
    {"naturalregeneration", "true"},
    {"playerssleepingpercentage", "100"},
    {"playerwaypoints", "1"},
    {"projectilescanbreakblocks", "true"},
    {"pvp", "true"},
    {"randomtickspeed", "1"},
    {"recipesunlock", "true"},
    {"respawnblocksexplode", "true"},
    {"sendcommandfeedback", "true"},
    {"showbordereffect", "true"},
    {"showcoordinates", "false"},
    {"showdaysplayed", "false"},
    {"showdeathmessages", "true"},
    {"showrecipemessages", "true"},
    {"showtags", "true"},
    {"spawnradius", "10"},
    {"tntexplodes", "true"},
    {"tntexplosiondropdecay", "false"},
};

struct ExactHistoricalDefaultOverride {
    std::string_view name;
    std::string_view version;
    std::string_view value;
};

struct HistoricalDefaultChangePoint {
    std::string_view name;
    std::string_view changed_in;
    std::string_view previous_value;
};

// These tables are intentionally sparse. Exact preview exceptions and known
// default-change boundaries are recorded instead of maintaining snapshots for
// every Bedrock version.
inline constexpr ExactHistoricalDefaultOverride kExactHistoricalOverrides[] = {
    {"recipesunlock", "1.20.30.21", "false"},
};

inline constexpr HistoricalDefaultChangePoint kHistoricalChangePoints[] = {
    {"spawnradius", "1.20.40", "5"},
};

// Renames/type changes are metadata only. They do not imply a default value.
inline constexpr GameRuleMigration kMigrations[] = {
    {"locatorbar", "playerwaypoints", "26.30", GameRuleValueKind::Boolean, GameRuleValueKind::Enum},
};

}  // namespace detail

inline std::optional<std::string_view> currentGameRuleFallback(std::string_view name)
{
    const std::string normalized = detail::normalizeGameRuleName(name);
    const auto it = std::find_if(std::begin(detail::kCurrentDefaults), std::end(detail::kCurrentDefaults),
                                 [&normalized](const auto &entry) { return entry.first == normalized; });
    if (it == std::end(detail::kCurrentDefaults)) {
        return std::nullopt;
    }
    return it->second;
}

inline std::optional<std::string> resolveGameRuleDefault(std::string_view name, std::string_view minecraft_version,
                                                         std::optional<std::string_view> runtime_default = std::nullopt)
{
    // Runtime/API knowledge is authoritative whenever the platform can provide it.
    if (runtime_default.has_value()) {
        return std::string(*runtime_default);
    }

    const std::string normalized_name = detail::normalizeGameRuleName(name);
    const std::string normalized_version = detail::normalizeMinecraftVersion(minecraft_version);

    if (!normalized_version.empty()) {
        const auto exact =
            std::find_if(std::begin(detail::kExactHistoricalOverrides), std::end(detail::kExactHistoricalOverrides),
                         [&normalized_name, &normalized_version](const auto &entry) {
                             return entry.name == normalized_name && entry.version == normalized_version;
                         });
        if (exact != std::end(detail::kExactHistoricalOverrides)) {
            return std::string(exact->value);
        }

        const auto change_point =
            std::find_if(std::begin(detail::kHistoricalChangePoints), std::end(detail::kHistoricalChangePoints),
                         [&normalized_name, &normalized_version](const auto &entry) {
                             return entry.name == normalized_name &&
                                    detail::compareMinecraftVersions(normalized_version, entry.changed_in) < 0;
                         });
        if (change_point != std::end(detail::kHistoricalChangePoints)) {
            return std::string(change_point->previous_value);
        }
    }

    const auto fallback = currentGameRuleFallback(normalized_name);
    if (!fallback.has_value()) {
        return std::nullopt;
    }
    return std::string(*fallback);
}

inline std::optional<GameRuleMigration> gameRuleMigration(std::string_view name)
{
    const std::string normalized = detail::normalizeGameRuleName(name);
    const auto it =
        std::find_if(std::begin(detail::kMigrations), std::end(detail::kMigrations), [&normalized](const auto &entry) {
            return entry.old_name == normalized || entry.new_name == normalized;
        });
    if (it == std::end(detail::kMigrations)) {
        return std::nullopt;
    }
    return *it;
}

}  // namespace spark
