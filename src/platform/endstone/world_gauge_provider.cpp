#include <chrono>
#include <string>

#include "platform/endstone/adapters.h"

namespace spark::endstone_adapter {

namespace {

constexpr std::int64_t KReconcileIntervalMs = 30000;

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

WorldGaugeChunkKey chunkKey(const ::endstone::Chunk &chunk)
{
    return {.dimension = std::string(chunk.getDimension()->getId()), .x = chunk.getX(), .z = chunk.getZ()};
}

}  // namespace

void EndstoneWorldGaugeProvider::init()
{
    if (initialized_) {
        return;
    }
    initialized_ = true;

    plugin_.registerEvent<::endstone::ActorSpawnEvent>(
        [this](::endstone::ActorSpawnEvent &event) { state_.actorSpawned(event.getActor()->getId()); },
        ::endstone::EventPriority::Monitor, true);

    plugin_.registerEvent<::endstone::ActorRemoveEvent>(
        [this](::endstone::ActorRemoveEvent &event) { state_.actorRemoved(event.getActor()->getId()); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::PlayerJoinEvent>(
        [this](::endstone::PlayerJoinEvent &event) { state_.playerSpawned(event.getPlayer()->getId()); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::PlayerQuitEvent>(
        [this](::endstone::PlayerQuitEvent &event) { state_.playerRemoved(event.getPlayer()->getId()); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ChunkLoadEvent>(
        [this](::endstone::ChunkLoadEvent &event) { state_.chunkLoaded(chunkKey(event.getChunk())); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ChunkUnloadEvent>(
        [this](::endstone::ChunkUnloadEvent &event) { state_.chunkUnloaded(chunkKey(event.getChunk())); },
        ::endstone::EventPriority::Monitor);

    reconcile();
}

std::pair<int, int> EndstoneWorldGaugeProvider::worldGauges()
{
    const std::int64_t now = steadyNowMs();
    if (now - last_reconcile_steady_ms_ >= KReconcileIntervalMs) {
        reconcile();
    }
    const WorldGaugeCounts counts = state_.counts();
    return {counts.entities, counts.chunks};
}

void EndstoneWorldGaugeProvider::reconcile()
{
    last_reconcile_steady_ms_ = steadyNowMs();

    WorldGaugeSnapshot snapshot;
    ::endstone::Level &level = server_.getLevel();
    for (const auto &dimension : level.getDimensions()) {
        for (const auto &actor : dimension->getActors()) {
            snapshot.actor_ids.push_back(actor->getId());
        }
        for (const auto &chunk : dimension->getLoadedChunks()) {
            snapshot.chunks.push_back(chunkKey(*chunk));
        }
    }
    for (const auto &player : server_.getOnlinePlayers()) {
        snapshot.player_ids.push_back(player->getId());
    }
    state_.reconcile(snapshot);
}

}  // namespace spark::endstone_adapter
