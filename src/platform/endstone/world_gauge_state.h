#ifndef SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_STATE_H
#define SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_STATE_H

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spark::endstone_adapter {

struct WorldGaugeChunkKey {
    std::string dimension;
    int x = 0;
    int z = 0;

    auto operator<=>(const WorldGaugeChunkKey &) const = default;
};

struct WorldGaugeSnapshot {
    std::vector<std::int64_t> actor_ids;
    std::vector<std::int64_t> player_ids;
    std::vector<WorldGaugeChunkKey> chunks;
};

struct WorldGaugeCounts {
    int players = 0;
    int entities = 0;
    int chunks = 0;

    bool operator==(const WorldGaugeCounts &) const = default;
};

class EndstoneWorldGaugeState {
public:
    void actorSpawned(std::int64_t actor_id)
    {
        std::scoped_lock lock(mutex_);
        actor_ids_.insert(actor_id);
    }

    void actorRemoved(std::int64_t actor_id)
    {
        std::scoped_lock lock(mutex_);
        actor_ids_.erase(actor_id);
    }

    void playerSpawned(std::int64_t player_id)
    {
        std::scoped_lock lock(mutex_);
        player_ids_.insert(player_id);
    }

    void playerRemoved(std::int64_t player_id)
    {
        std::scoped_lock lock(mutex_);
        player_ids_.erase(player_id);
        actor_ids_.erase(player_id);
    }

    void chunkLoaded(WorldGaugeChunkKey key)
    {
        std::scoped_lock lock(mutex_);
        chunks_.insert(std::move(key));
    }

    void chunkUnloaded(const WorldGaugeChunkKey &key)
    {
        std::scoped_lock lock(mutex_);
        chunks_.erase(key);
    }

    void reconcile(const WorldGaugeSnapshot &snapshot)
    {
        std::set<std::int64_t> actors(snapshot.actor_ids.begin(), snapshot.actor_ids.end());
        std::set<std::int64_t> players(snapshot.player_ids.begin(), snapshot.player_ids.end());
        std::set<WorldGaugeChunkKey> chunks(snapshot.chunks.begin(), snapshot.chunks.end());

        std::scoped_lock lock(mutex_);
        actor_ids_ = std::move(actors);
        player_ids_ = std::move(players);
        chunks_ = std::move(chunks);
    }

    [[nodiscard]] WorldGaugeCounts counts() const
    {
        std::scoped_lock lock(mutex_);
        std::size_t entity_count = actor_ids_.size();
        for (const std::int64_t player_id : player_ids_) {
            if (!actor_ids_.contains(player_id)) {
                ++entity_count;
            }
        }
        return {.players = static_cast<int>(player_ids_.size()),
                .entities = static_cast<int>(entity_count),
                .chunks = static_cast<int>(chunks_.size())};
    }

private:
    mutable std::mutex mutex_;
    std::set<std::int64_t> actor_ids_;
    std::set<std::int64_t> player_ids_;
    std::set<WorldGaugeChunkKey> chunks_;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_STATE_H
