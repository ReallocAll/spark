#ifndef SPARK_PLATFORM_LEVILAMINA_BDS_PLAYER_PING_H
#define SPARK_PLATFORM_LEVILAMINA_BDS_PLAYER_PING_H

#include <cstdint>
#include <optional>

class Player;

namespace spark::levilamina::bds {

std::optional<std::int64_t> readPlayerAveragePingMilliseconds(Player const &player);

}  // namespace spark::levilamina::bds

#endif  // SPARK_PLATFORM_LEVILAMINA_BDS_PLAYER_PING_H
