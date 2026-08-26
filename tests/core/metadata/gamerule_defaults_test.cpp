#include <cassert>
#include <string>

#include "core/metadata/gamerule_defaults.h"

int main()
{
    using spark::GameRuleValueKind;
    using spark::gameRuleMigration;
    using spark::resolveGameRuleDefault;

    // Runtime/API capability always wins over the built-in fallback.
    assert(resolveGameRuleDefault("dofiretick", "26.44", "false") == std::optional<std::string>{"false"});

    // Current fallback values come from a fresh Bedrock 1.26.44.3 baseline.
    assert(resolveGameRuleDefault("dofiretick", "26.44") == std::optional<std::string>{"true"});
    assert(resolveGameRuleDefault("minecraft:spawnradius", "26.44") == std::optional<std::string>{"10"});
    assert(resolveGameRuleDefault("maxcommandchainlength", "1.26.44.3") == std::optional<std::string>{"65535"});
    assert(resolveGameRuleDefault("tntexplosiondropdecay", "26.44") == std::optional<std::string>{"false"});

    // Historical changes are sparse overrides, not version snapshots.
    assert(resolveGameRuleDefault("spawnradius", "1.20.30") == std::optional<std::string>{"5"});
    assert(resolveGameRuleDefault("spawnradius", "1.20.40") == std::optional<std::string>{"10"});
    assert(resolveGameRuleDefault("spawnradius", "1.20.40.20") == std::optional<std::string>{"10"});
    assert(resolveGameRuleDefault("recipesunlock", "1.20.30.20") == std::optional<std::string>{"false"});
    assert(resolveGameRuleDefault("recipesunlock", "1.20.30.21") == std::optional<std::string>{"false"});
    assert(resolveGameRuleDefault("recipesunlock", "1.20.30.22") == std::optional<std::string>{"true"});
    assert(resolveGameRuleDefault("recipesunlock", "1.20.30") == std::optional<std::string>{"true"});

    // Unknown/new rules must never inherit the effective world value or a guessed default.
    assert(!resolveGameRuleDefault("futureBrandNewRule", "26.44").has_value());

    // The locator-bar migration changes both name and type. Keep it separate
    // from default lookup, especially while Endstone exposes playerwaypoints as int.
    assert(!resolveGameRuleDefault("locatorbar", "26.44").has_value());
    assert(!resolveGameRuleDefault("playerwaypoints", "26.44").has_value());
    const auto migration = gameRuleMigration("locatorbar", "26.44");
    assert(migration.has_value());
    assert(migration->old_name == "locatorbar");
    assert(migration->new_name == "playerwaypoints");
    assert(migration->since_version == "26.30");
    assert(migration->old_kind == GameRuleValueKind::Boolean);
    assert(migration->new_kind == GameRuleValueKind::Enumeration);
    assert(gameRuleMigration("playerwaypoints", "1.21.100") == std::nullopt);

    return 0;
}
