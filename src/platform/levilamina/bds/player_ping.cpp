#include "platform/levilamina/bds/player_ping.h"

#include "mc/world/actor/player/Player.h"

namespace spark::levilamina::bds {

std::optional<std::int64_t> readPlayerAveragePingMilliseconds(Player const &player)
{
    const auto status = player.getNetworkStatus();
    if (!status.has_value()) {
        return std::nullopt;
    }
    return status->mAveragePing.get().count();
}

}  // namespace spark::levilamina::bds
