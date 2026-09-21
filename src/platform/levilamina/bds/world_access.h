// Copyright (c) 2024, The Endstone Project. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SPARK_PLATFORM_LEVILAMINA_BDS_WORLD_ACCESS_H
#define SPARK_PLATFORM_LEVILAMINA_BDS_WORLD_ACCESS_H

#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "core/metadata/platform_metadata.h"
#include "mc/deps/game_refs/WeakRef.h"
#include "mc/world/level/chunk/ChunkState.h"
#include "platform/endstone/world_gauge_state.h"

class Dimension;
class Level;
class LevelChunk;

namespace spark::levilamina::bds {

using WorldGaugeChunkKey = ::spark::endstone_adapter::WorldGaugeChunkKey;
using WorldGaugeSnapshot = ::spark::endstone_adapter::WorldGaugeSnapshot;

enum class DimensionRetention {
    Retained,
    ForeignLevel,
    Failed,
};

enum class ChunkKeyResult {
    Valid,
    ForeignLevel,
    Failed,
};

namespace detail {

[[nodiscard]] inline bool isLoadedChunkState(::ChunkState state) noexcept
{
    return state >= ::ChunkState::Loaded;
}

[[nodiscard]] inline bool dimensionCaptureInvalidates(DimensionRetention result) noexcept
{
    return result == DimensionRetention::Failed;
}

[[nodiscard]] inline bool floorChunkCoordinate(float value, int &result) noexcept
{
    if (!std::isfinite(value)) {
        return false;
    }
    const double coordinate = std::floor(static_cast<double>(value) / 16.0);
    if (coordinate < static_cast<double>((std::numeric_limits<int>::min)()) ||
        coordinate > static_cast<double>((std::numeric_limits<int>::max)())) {
        return false;
    }
    result = static_cast<int>(coordinate);
    return true;
}

}  // namespace detail

[[nodiscard]] std::string canonicalDimensionName(int dimension_id);

class WorldAccess final {
public:
    struct ScanResult {
        WorldGaugeSnapshot gauges;
        WorldInfo world;
    };

    explicit WorldAccess(::Level &level) noexcept : level_(&level) {}

    WorldAccess(WorldAccess const &) = delete;
    WorldAccess &operator=(WorldAccess const &) = delete;

    [[nodiscard]] ::Level *level() const noexcept { return level_; }

    // Called by the dimension-created callback and by the initial enumeration.
    // The borrowed Dimension reference is consumed before this function returns.
    [[nodiscard]] DimensionRetention retainDimension(::Dimension &dimension) noexcept;

    // Captures one complete, main-thread snapshot. No BDS reference leaves this
    // call; weak dimensions and chunks are locked only for the current scan.
    [[nodiscard]] bool scan(std::string_view level_name_hint, ScanResult &result) noexcept;

    // Converts a live chunk callback argument into the stable key used by the
    // shared mutex-protected gauge state.
    [[nodiscard]] ChunkKeyResult chunkKeyStatus(::LevelChunk const &chunk, WorldGaugeChunkKey &key) const noexcept;
    [[nodiscard]] bool chunkKey(::LevelChunk const &chunk, WorldGaugeChunkKey &key) const noexcept
    {
        return chunkKeyStatus(chunk, key) == ChunkKeyResult::Valid;
    }

private:
    bool retainKnownDimensions() noexcept;
    bool pruneExpiredDimensions() noexcept;

    ::Level *level_ = nullptr;
    std::map<int, ::WeakRef<::Dimension>> dimensions_;
};

}  // namespace spark::levilamina::bds

#endif  // SPARK_PLATFORM_LEVILAMINA_BDS_WORLD_ACCESS_H
