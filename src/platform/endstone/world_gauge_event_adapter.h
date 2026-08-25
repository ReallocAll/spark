#ifndef SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_EVENT_ADAPTER_H
#define SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_EVENT_ADAPTER_H

#include <cstdint>
#include <utility>

#include "platform/endstone/world_gauge_state.h"

namespace spark::endstone_adapter {

// Production seam shared by Endstone event callbacks and deterministic tests.
class EndstoneWorldGaugeEventAdapter {
public:
    void actorSpawned(std::int64_t actor_id) { state_.actorSpawned(actor_id); }
    void actorRemoved(std::int64_t actor_id) { state_.actorRemoved(actor_id); }
    void playerSpawned(std::int64_t player_id) { state_.playerSpawned(player_id); }
    void playerRemoved(std::int64_t player_id) { state_.playerRemoved(player_id); }
    void chunkLoaded(WorldGaugeChunkKey key) { state_.chunkLoaded(std::move(key)); }
    void chunkUnloaded(const WorldGaugeChunkKey &key) { state_.chunkUnloaded(key); }
    void reconcile(const WorldGaugeSnapshot &snapshot) { state_.reconcile(snapshot); }

    [[nodiscard]] WorldGaugeCounts counts() const { return state_.counts(); }

private:
    EndstoneWorldGaugeState state_;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_EVENT_ADAPTER_H
