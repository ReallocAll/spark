#include <chrono>

#include "platform/endstone/adapters.h"

namespace spark::endstone_adapter {

namespace {

constexpr std::int64_t KReconcileIntervalMs = 30000;

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void EndstoneWorldGaugeProvider::init()
{
    if (initialized_) {
        return;
    }
    initialized_ = true;

    plugin_.registerEvent<::endstone::ActorSpawnEvent>(
        [this](::endstone::ActorSpawnEvent &) { entity_count_.fetch_add(1, std::memory_order_relaxed); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ActorRemoveEvent>(
        [this](::endstone::ActorRemoveEvent &) { entity_count_.fetch_sub(1, std::memory_order_relaxed); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ChunkLoadEvent>(
        [this](::endstone::ChunkLoadEvent &) { chunk_count_.fetch_add(1, std::memory_order_relaxed); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ChunkUnloadEvent>(
        [this](::endstone::ChunkUnloadEvent &) { chunk_count_.fetch_sub(1, std::memory_order_relaxed); },
        ::endstone::EventPriority::Monitor);

    reconcile();
}

std::pair<int, int> EndstoneWorldGaugeProvider::worldGauges()
{
    std::int64_t now = steadyNowMs();
    if (now - last_reconcile_steady_ms_ >= KReconcileIntervalMs) {
        reconcile();
    }
    return {entity_count_.load(std::memory_order_relaxed), chunk_count_.load(std::memory_order_relaxed)};
}

void EndstoneWorldGaugeProvider::reconcile()
{
    last_reconcile_steady_ms_ = steadyNowMs();

    int entities = 0;
    int chunks = 0;
    ::endstone::Level &level = server_.getLevel();
    for (const auto &dimension : level.getDimensions()) {
        for (const auto &actor : dimension->getActors()) {
            ++entities;
        }
        chunks += static_cast<int>(dimension->getLoadedChunks().size());
    }
    entity_count_.store(entities, std::memory_order_relaxed);
    chunk_count_.store(chunks, std::memory_order_relaxed);
}

}  // namespace spark::endstone_adapter
