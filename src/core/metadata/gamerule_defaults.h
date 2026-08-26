#ifndef ENDSTONE_SPARK_GAMERULE_DEFAULTS_H
#define ENDSTONE_SPARK_GAMERULE_DEFAULTS_H

#include <optional>
#include <string>
#include <string_view>

namespace spark {

enum class GameRuleValueKind {
    Boolean,
    Integer,
    Enumeration,
};

struct GameRuleMigration {
    std::string_view old_name;
    std::string_view new_name;
    std::string_view since_version;
    GameRuleValueKind old_kind;
    GameRuleValueKind new_kind;
};

// Resolve a Gamerule default without inferring it from the world's effective
// value. A runtime/API-provided default always wins. If no such capability is
// available, sparse historical overrides are applied before the current
// Bedrock fallback table. Unknown/new rules remain std::nullopt.
std::optional<std::string> resolveGameRuleDefault(
    std::string_view name, std::string_view minecraft_version,
    std::optional<std::string_view> runtime_default = std::nullopt);

// Rename/type migrations are intentionally modeled separately from default
// changes. This prevents enum migrations from being mistaken for a boolean or
// integer default-value change.
std::optional<GameRuleMigration> gameRuleMigration(std::string_view name, std::string_view minecraft_version);

}  // namespace spark

#endif  // ENDSTONE_SPARK_GAMERULE_DEFAULTS_H
