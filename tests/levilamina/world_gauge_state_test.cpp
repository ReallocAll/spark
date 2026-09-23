#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "platform/endstone/world_gauge_event_adapter.h"

namespace {

using spark::endstone_adapter::EndstoneWorldGaugeEventAdapter;
using spark::endstone_adapter::WorldGaugeChunkKey;
using spark::endstone_adapter::WorldGaugeCounts;
using spark::endstone_adapter::WorldGaugeSnapshot;

void require(bool condition, char const *message)
{
    if (!condition) {
        throw std::runtime_error{message};
    }
}

WorldGaugeChunkKey chunk(std::string dimension, int x, int z)
{
    return {.dimension = std::move(dimension), .x = x, .z = z};
}

void requireCounts(EndstoneWorldGaugeEventAdapter const &adapter, WorldGaugeCounts expected)
{
    require(adapter.counts() == expected, "world gauge counts differ from the expected set state");
}

void testDuplicateEventsAndPlayerDeduplication()
{
    EndstoneWorldGaugeEventAdapter state;
    state.playerSpawned(7);
    state.playerSpawned(7);
    state.actorSpawned(7);
    state.actorSpawned(7);
    state.actorSpawned(8);
    state.actorSpawned(8);
    state.chunkLoaded(chunk("overworld", 1, 2));
    state.chunkLoaded(chunk("overworld", 1, 2));
    requireCounts(state, {.players = 1, .entities = 2, .tile_entities = 0, .chunks = 1});

    state.playerRemoved(7);
    state.actorRemoved(8);
    state.chunkUnloaded(chunk("overworld", 1, 2));
    requireCounts(state, {.players = 0, .entities = 0, .tile_entities = 0, .chunks = 0});
}

void testSnapshotReplacementAndTileAbsence()
{
    EndstoneWorldGaugeEventAdapter state;
    const auto overworld = chunk("overworld", 4, -3);
    const auto nether = chunk("nether", 4, -3);

    WorldGaugeSnapshot first;
    first.actor_ids = {10, 11, 11};
    first.player_ids = {10, 10};
    first.chunks = {overworld, overworld};
    state.reconcile(first);
    requireCounts(state,
                  {.players = 1, .entities = 2, .tile_entities = 0, .chunks = 1, .tile_entities_present = false});

    WorldGaugeSnapshot replacement;
    replacement.actor_ids = {12};
    replacement.player_ids = {};
    replacement.chunks = {nether};
    state.reconcile(replacement);
    requireCounts(state,
                  {.players = 0, .entities = 1, .tile_entities = 0, .chunks = 1, .tile_entities_present = false});
}

void testCompleteTileReconciliationIsExplicit()
{
    EndstoneWorldGaugeEventAdapter state;
    WorldGaugeSnapshot snapshot;
    snapshot.chunks = {chunk("overworld", 0, 0)};
    snapshot.tile_entities_complete = true;
    snapshot.tile_entities.push_back({.chunk = snapshot.chunks.front(), .count = 3});
    state.reconcile(snapshot);
    requireCounts(state, {.players = 0, .entities = 0, .tile_entities = 3, .chunks = 1, .tile_entities_present = true});

    WorldGaugeSnapshot without_tiles;
    without_tiles.chunks = snapshot.chunks;
    state.reconcile(without_tiles);
    requireCounts(state, {.players = 0, .entities = 0, .tile_entities = 3, .chunks = 1, .tile_entities_present = true});
}

}  // namespace

int main()
{
    try {
        testDuplicateEventsAndPlayerDeduplication();
        testSnapshotReplacementAndTileAbsence();
        testCompleteTileReconciliationIsExplicit();
        return 0;
    }
    catch (...) {
        return 1;
    }
}
