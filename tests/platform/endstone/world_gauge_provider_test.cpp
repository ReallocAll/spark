#include <cassert>
#include <cstdint>
#include <string>

#include "platform/endstone/world_gauge_state.h"

namespace {

using spark::endstone_adapter::EndstoneWorldGaugeState;
using spark::endstone_adapter::WorldGaugeChunkKey;
using spark::endstone_adapter::WorldGaugeCounts;
using spark::endstone_adapter::WorldGaugeSnapshot;

WorldGaugeChunkKey chunk(std::string dimension, int x, int z)
{
    return {.dimension = std::move(dimension), .x = x, .z = z};
}

void expectCounts(const EndstoneWorldGaugeState &state, int players, int entities, int chunks)
{
    assert(state.counts() == WorldGaugeCounts{.players = players, .entities = entities, .chunks = chunks});
}

}  // namespace

int main()
{
    constexpr std::int64_t KPlayer = 100;
    constexpr std::int64_t KEntity = 200;
    constexpr std::int64_t KSecondEntity = 300;

    EndstoneWorldGaugeState lifecycle;
    expectCounts(lifecycle, 0, 0, 0);

    lifecycle.playerSpawned(KPlayer);
    expectCounts(lifecycle, 1, 1, 0);

    // ActorSpawnEvent may also be observed for the same player. Identity tracking must prevent double counting.
    lifecycle.actorSpawned(KPlayer);
    lifecycle.playerSpawned(KPlayer);
    expectCounts(lifecycle, 1, 1, 0);

    lifecycle.actorSpawned(KEntity);
    lifecycle.actorSpawned(KEntity);
    expectCounts(lifecycle, 1, 2, 0);

    lifecycle.actorRemoved(KEntity);
    lifecycle.actorRemoved(KEntity);
    expectCounts(lifecycle, 1, 1, 0);

    // Endstone documents that ActorRemoveEvent is not fired for players, so PlayerQuitEvent must remove both paths.
    lifecycle.playerRemoved(KPlayer);
    lifecycle.playerRemoved(KPlayer);
    expectCounts(lifecycle, 0, 0, 0);

    const WorldGaugeChunkKey overworld = chunk("minecraft:overworld", 4, -7);
    const WorldGaugeChunkKey nether_same_coordinates = chunk("minecraft:nether", 4, -7);
    lifecycle.chunkLoaded(overworld);
    lifecycle.chunkLoaded(overworld);
    lifecycle.chunkLoaded(nether_same_coordinates);
    expectCounts(lifecycle, 0, 0, 2);

    lifecycle.chunkUnloaded(overworld);
    lifecycle.chunkUnloaded(overworld);
    expectCounts(lifecycle, 0, 0, 1);
    lifecycle.chunkUnloaded(nether_same_coordinates);
    expectCounts(lifecycle, 0, 0, 0);

    // Reconciliation must repair both missed removals and missed spawns while deduplicating players present as actors.
    EndstoneWorldGaugeState reconciled;
    reconciled.playerSpawned(KPlayer);
    reconciled.actorSpawned(KPlayer);
    reconciled.actorSpawned(KEntity);
    reconciled.actorSpawned(KSecondEntity);
    reconciled.chunkLoaded(overworld);
    expectCounts(reconciled, 1, 3, 1);

    WorldGaugeSnapshot repaired;
    repaired.actor_ids = {KPlayer, KSecondEntity, KSecondEntity};
    repaired.player_ids = {KPlayer, KPlayer};
    repaired.chunks = {nether_same_coordinates, nether_same_coordinates};
    reconciled.reconcile(repaired);
    expectCounts(reconciled, 1, 2, 1);

    // A full scan and the equivalent incremental event sequence must converge to exactly the same gauges.
    WorldGaugeSnapshot initial_scan;
    initial_scan.actor_ids = {KPlayer, KEntity};
    initial_scan.player_ids = {KPlayer};
    initial_scan.chunks = {overworld, nether_same_coordinates};

    EndstoneWorldGaugeState from_scan;
    from_scan.reconcile(initial_scan);

    EndstoneWorldGaugeState from_events;
    from_events.playerSpawned(KPlayer);
    from_events.actorSpawned(KPlayer);
    from_events.actorSpawned(KEntity);
    from_events.chunkLoaded(overworld);
    from_events.chunkLoaded(nether_same_coordinates);
    assert(from_scan.counts() == from_events.counts());
    expectCounts(from_scan, 1, 2, 2);

    // Reconnects and state resets must not preserve stale identities or allow counters to underflow.
    from_events.playerRemoved(KPlayer);
    expectCounts(from_events, 0, 1, 2);
    from_events.playerSpawned(KPlayer);
    expectCounts(from_events, 1, 2, 2);
    from_events.reconcile(WorldGaugeSnapshot{});
    expectCounts(from_events, 0, 0, 0);
    from_events.actorRemoved(KEntity);
    from_events.playerRemoved(KPlayer);
    from_events.chunkUnloaded(overworld);
    expectCounts(from_events, 0, 0, 0);
}
