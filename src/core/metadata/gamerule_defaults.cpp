#include "core/metadata/gamerule_defaults.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace spark {
namespace {

struct DefaultEntry {
    std::string_view name;
    std::string_view value;
};

// Current Bedrock defaults measured against a fresh BDS 1.26.44.3 world.
// Keep this as one current baseline, not a per-version snapshot matrix.
constexpr std::array KCurrentDefaults{
    DefaultEntry{"commandblockoutput", "true"},
    DefaultEntry{"commandblocksenabled", "true"},
    DefaultEntry{"dodaylightcycle", "true"},
    DefaultEntry{"doentitydrops", "true"},
    DefaultEntry{"dofiretick", "true"},
    DefaultEntry{"doimmediaterespawn", "false"},
    DefaultEntry{"doinsomnia", "true"},
    DefaultEntry{"dolimitedcrafting", "false"},
    DefaultEntry{"domobloot", "true"},
    DefaultEntry{"domobspawning", "true"},
    DefaultEntry{"dotiledrops", "true"},
    DefaultEntry{"doweathercycle", "true"},
    DefaultEntry{"drowningdamage", "true"},
    DefaultEntry{"falldamage", "true"},
    DefaultEntry{"firedamage", "true"},
    DefaultEntry{"freezedamage", "true"},
    DefaultEntry{"functioncommandlimit", "10000"},
    DefaultEntry{"keepinventory", "false"},
    DefaultEntry{"maxcommandchainlength", "65535"},
    DefaultEntry{"mobgriefing", "true"},
    DefaultEntry{"naturalregeneration", "true"},
    DefaultEntry{"playerssleepingpercentage", "100"},
    DefaultEntry{"projectilescanbreakblocks", "true"},
    DefaultEntry{"pvp", "true"},
    DefaultEntry{"randomtickspeed", "1"},
    DefaultEntry{"recipesunlock", "true"},
    DefaultEntry{"respawnblocksexplode", "true"},
    DefaultEntry{"sendcommandfeedback", "true"},
    DefaultEntry{"showbordereffect", "true"},
    DefaultEntry{"showcoordinates", "false"},
    DefaultEntry{"showdaysplayed", "false"},
    DefaultEntry{"showdeathmessages", "true"},
    DefaultEntry{"showrecipemessages", "true"},
    DefaultEntry{"showtags", "true"},
    DefaultEntry{"spawnradius", "10"},
    DefaultEntry{"tntexplodes", "true"},
    DefaultEntry{"tntexplosiondropdecay", "false"},
};

std::string normalizeName(std::string_view name)
{
    constexpr std::string_view minecraft_prefix = "minecraft:";
    if (name.starts_with(minecraft_prefix)) {
        name.remove_prefix(minecraft_prefix.size());
    }
    std::string result(name);
    std::ranges::transform(result, result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

std::vector<int> parseVersion(std::string_view version)
{
    std::vector<int> parts;
    while (!version.empty()) {
        const std::size_t separator = version.find('.');
        const std::string_view part = version.substr(0, separator);
        int value = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) {
            return {};
        }
        parts.push_back(value);
        if (separator == std::string_view::npos) {
            break;
        }
        version.remove_prefix(separator + 1);
    }
    return parts;
}

bool isBeforeBedrock12040(std::string_view version)
{
    const std::vector<int> parts = parseVersion(version);
    if (parts.size() < 3 || parts[0] != 1) {
        return false;
    }
    if (parts[1] != 20) {
        return parts[1] < 20;
    }
    return parts[2] < 40;
}

bool isRecipesUnlockFalsePreview(std::string_view version)
{
    const std::vector<int> parts = parseVersion(version);
    return parts.size() >= 4 && parts[0] == 1 && parts[1] == 20 && parts[2] == 30 &&
           (parts[3] == 20 || parts[3] == 21);
}

bool isAtLeast26_30(std::string_view version)
{
    const std::vector<int> parts = parseVersion(version);
    if (parts.empty()) {
        return false;
    }
    if (parts[0] == 1) {
        if (parts.size() < 3) {
            return false;
        }
        return parts[1] > 26 || (parts[1] == 26 && parts[2] >= 30);
    }
    if (parts.size() < 2) {
        return false;
    }
    return parts[0] > 26 || (parts[0] == 26 && parts[1] >= 30);
}

std::optional<std::string> sparseHistoricalDefault(std::string_view name, std::string_view version)
{
    if (name == "spawnradius" && isBeforeBedrock12040(version)) {
        return "5";
    }
    if (name == "recipesunlock" && isRecipesUnlockFalsePreview(version)) {
        return "false";
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> resolveGameRuleDefault(std::string_view name, std::string_view minecraft_version,
                                                  std::optional<std::string_view> runtime_default)
{
    if (runtime_default.has_value()) {
        return std::string(*runtime_default);
    }

    const std::string normalized = normalizeName(name);
    if (const auto historical = sparseHistoricalDefault(normalized, minecraft_version); historical.has_value()) {
        return historical;
    }

    // locatorbar -> playerwaypoints changes both the name and value type. The
    // current Endstone API still exposes playerwaypoints as an int while BDS
    // exposes enum values (off/everyone), so do not guess an integer default.
    if (normalized == "locatorbar" || normalized == "playerwaypoints") {
        return std::nullopt;
    }

    for (const DefaultEntry &entry : KCurrentDefaults) {
        if (entry.name == normalized) {
            return std::string(entry.value);
        }
    }
    return std::nullopt;
}

std::optional<GameRuleMigration> gameRuleMigration(std::string_view name, std::string_view minecraft_version)
{
    const std::string normalized = normalizeName(name);
    if ((normalized == "locatorbar" || normalized == "playerwaypoints") && isAtLeast26_30(minecraft_version)) {
        return GameRuleMigration{
            .old_name = "locatorbar",
            .new_name = "playerwaypoints",
            .since_version = "26.30",
            .old_kind = GameRuleValueKind::Boolean,
            .new_kind = GameRuleValueKind::Enumeration,
        };
    }
    return std::nullopt;
}

}  // namespace spark
