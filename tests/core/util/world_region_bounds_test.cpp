#include <cassert>
#include <limits>
#include <map>
#include <utility>

#include "core/util/world_region.h"

namespace {

spark::WorldChunk makeChunk(int x, int z, int entities)
{
    spark::WorldChunk chunk;
    chunk.x = x;
    chunk.z = z;
    chunk.total_entities = entities;
    return chunk;
}

}  // namespace

int main()
{
    const int minimum = std::numeric_limits<int>::min();
    const int maximum = std::numeric_limits<int>::max();

    {
        std::map<std::pair<int, int>, spark::WorldChunk> chunks;
        chunks[{minimum, 0}] = makeChunk(minimum, 0, 1);
        chunks[{maximum, 0}] = makeChunk(maximum, 0, 1);
        const auto regions = spark::groupChunksIntoRegions(chunks);
        assert(regions.size() == 2);
    }
    {
        std::map<std::pair<int, int>, spark::WorldChunk> chunks;
        chunks[{maximum - 1, 0}] = makeChunk(maximum - 1, 0, 1);
        chunks[{maximum, 0}] = makeChunk(maximum, 0, 1);
        const auto regions = spark::groupChunksIntoRegions(chunks);
        assert(regions.size() == 1);
        assert(regions.front().chunks.size() == 2);
    }
    {
        std::map<std::pair<int, int>, spark::WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, maximum);
        chunks[{1, 0}] = makeChunk(1, 0, maximum);
        const auto regions = spark::groupChunksIntoRegions(chunks);
        assert(regions.size() == 1);
        assert(regions.front().total_entities == maximum);
    }
    return 0;
}
