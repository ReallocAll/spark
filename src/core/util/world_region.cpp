#include "core/util/world_region.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spark {

namespace {

// Packs a chunk coordinate into a 64-bit key for hash-based lookup.
// Handles negative coordinates correctly via unsigned masking.
std::uint64_t packCoord(int x, int z)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) << 32) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(x));
}

bool offsetCoordinate(int value, int delta, int &result)
{
    const std::int64_t candidate = static_cast<std::int64_t>(value) + delta;
    if (candidate < std::numeric_limits<int>::min() || candidate > std::numeric_limits<int>::max()) {
        return false;
    }
    result = static_cast<int>(candidate);
    return true;
}

int saturatingEntitySum(int left, int right)
{
    return right > std::numeric_limits<int>::max() - left ? std::numeric_limits<int>::max() : left + right;
}

}  // namespace

std::vector<WorldRegion> groupChunksIntoRegions(const std::map<std::pair<int, int>, WorldChunk> &chunks)
{
    // Build a hash map of chunks with entities > 0, keyed by packed coordinate.
    std::unordered_map<std::uint64_t, const WorldChunk *> chunk_map;
    chunk_map.reserve(chunks.size());
    for (const auto &[coord, chunk] : chunks) {
        if (chunk.total_entities <= 0) {
            continue;
        }
        chunk_map.emplace(packCoord(coord.first, coord.second), &chunk);
    }

    std::vector<WorldRegion> regions;
    std::unordered_map<std::uint64_t, bool> visited;
    visited.reserve(chunk_map.size());

    std::deque<const WorldChunk *> queue;

    for (const auto &[key, chunkPtr] : chunk_map) {
        if (visited[key]) {
            continue;
        }

        visited[key] = true;
        queue.push_back(chunkPtr);

        WorldRegion region;
        region.total_entities = chunkPtr->total_entities;
        region.chunks.push_back(*chunkPtr);

        while (!queue.empty()) {
            const WorldChunk *current = queue.front();
            queue.pop_front();
            int cx = current->x;
            int cz = current->z;

            // 8-neighbor adjacency: dx and dz from -1 to 1, skip (0,0).
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    int neighbor_x = 0;
                    int neighbor_z = 0;
                    if (!offsetCoordinate(cx, dx, neighbor_x) || !offsetCoordinate(cz, dz, neighbor_z)) {
                        continue;
                    }
                    std::uint64_t neighbor_key = packCoord(neighbor_x, neighbor_z);
                    auto it = chunk_map.find(neighbor_key);
                    if (it == chunk_map.end() || visited[neighbor_key]) {
                        continue;
                    }
                    visited[neighbor_key] = true;
                    region.total_entities = saturatingEntitySum(region.total_entities, it->second->total_entities);
                    region.chunks.push_back(*it->second);
                    queue.push_back(it->second);
                }
            }
        }

        regions.push_back(std::move(region));
    }

    return regions;
}

}  // namespace spark
