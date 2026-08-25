#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <endstone/plugin/plugin_description.h>

#include "platform/endstone/papi_integration.h"
#include "platform/endstone/world_gauge_state.h"

namespace {

class FakePlugin final : public endstone::Plugin {
public:
    [[nodiscard]] const endstone::PluginDescription &getDescription() const override { return description_; }

private:
    endstone::PluginDescription description_{"spark", "0.6.0"};
};

class FakePlaceholderApi final : public papi::PlaceholderAPI {
public:
    [[nodiscard]] bool isActive() const noexcept override { return active; }

    [[nodiscard]] std::string setPlaceholders(const endstone::OfflinePlayer *player,
                                              std::string_view text) const override
    {
        if (!active || !expansion || text != "{spark:unknown}") {
            return std::string(text);
        }
        auto value = expansion->onRequest(player, "unknown");
        return value.value_or(std::string(text));
    }

    [[nodiscard]] std::string setRelationalPlaceholders(const endstone::Player &, const endstone::Player &,
                                                        std::string_view text) const override
    {
        return std::string(text);
    }

    [[nodiscard]] bool containsPlaceholders(std::string_view text) const noexcept override
    {
        return text.find('{') != std::string_view::npos && text.find('}') != std::string_view::npos;
    }

    [[nodiscard]] bool isRegistered(std::string_view identifier) const override
    {
        return expansion && identifier == "spark";
    }

    [[nodiscard]] std::vector<std::string> getRegisteredIdentifiers() const override
    {
        return expansion ? std::vector<std::string>{"spark"} : std::vector<std::string>{};
    }

    [[nodiscard]] std::vector<papi::ExpansionInfo> getExpansions() const override { return {}; }

    bool registerExpansion(endstone::Plugin &owner, std::shared_ptr<papi::PlaceholderExpansion> candidate) override
    {
        ++register_calls;
        if (!active || reject_registration || expansion) {
            return false;
        }
        registered_owner = &owner;
        expansion = std::move(candidate);
        return true;
    }

    bool unregisterExpansion(endstone::Plugin &owner, std::string_view identifier) override
    {
        ++unregister_calls;
        if (!active || registered_owner != &owner || identifier != "spark" || !expansion) {
            return false;
        }
        expansion.reset();
        registered_owner = nullptr;
        return true;
    }

    std::size_t unregisterExpansions(endstone::Plugin &owner) override
    {
        return unregisterExpansion(owner, "spark") ? 1 : 0;
    }

    bool active = true;
    bool reject_registration = false;
    int register_calls = 0;
    int unregister_calls = 0;
    endstone::Plugin *registered_owner = nullptr;
    std::shared_ptr<papi::PlaceholderExpansion> expansion;
};

using spark::endstone_adapter::EndstoneWorldGaugeState;
using spark::endstone_adapter::WorldGaugeChunkKey;
using spark::endstone_adapter::WorldGaugeCounts;
using spark::endstone_adapter::WorldGaugeSnapshot;

WorldGaugeChunkKey chunk(std::string dimension, int x, int z)
{
    return {.dimension = std::move(dimension), .x = x, .z = z};
}

void expectGaugeCounts(const EndstoneWorldGaugeState &state, int players, int entities, int chunks)
{
    assert(state.counts() == WorldGaugeCounts{.players = players, .entities = entities, .chunks = chunks});
}

void testWorldGaugeLifecycle()
{
    constexpr std::int64_t KPlayer = 100;
    constexpr std::int64_t KEntity = 200;
    constexpr std::int64_t KSecondEntity = 300;

    EndstoneWorldGaugeState lifecycle;
    expectGaugeCounts(lifecycle, 0, 0, 0);

    lifecycle.playerSpawned(KPlayer);
    expectGaugeCounts(lifecycle, 1, 1, 0);

    lifecycle.actorSpawned(KPlayer);
    lifecycle.playerSpawned(KPlayer);
    expectGaugeCounts(lifecycle, 1, 1, 0);

    lifecycle.actorSpawned(KEntity);
    lifecycle.actorSpawned(KEntity);
    expectGaugeCounts(lifecycle, 1, 2, 0);

    lifecycle.actorRemoved(KEntity);
    lifecycle.actorRemoved(KEntity);
    expectGaugeCounts(lifecycle, 1, 1, 0);

    lifecycle.playerRemoved(KPlayer);
    lifecycle.playerRemoved(KPlayer);
    expectGaugeCounts(lifecycle, 0, 0, 0);

    const WorldGaugeChunkKey overworld = chunk("minecraft:overworld", 4, -7);
    const WorldGaugeChunkKey nether_same_coordinates = chunk("minecraft:nether", 4, -7);
    lifecycle.chunkLoaded(overworld);
    lifecycle.chunkLoaded(overworld);
    lifecycle.chunkLoaded(nether_same_coordinates);
    expectGaugeCounts(lifecycle, 0, 0, 2);

    lifecycle.chunkUnloaded(overworld);
    lifecycle.chunkUnloaded(overworld);
    expectGaugeCounts(lifecycle, 0, 0, 1);
    lifecycle.chunkUnloaded(nether_same_coordinates);
    expectGaugeCounts(lifecycle, 0, 0, 0);

    EndstoneWorldGaugeState reconciled;
    reconciled.playerSpawned(KPlayer);
    reconciled.actorSpawned(KPlayer);
    reconciled.actorSpawned(KEntity);
    reconciled.actorSpawned(KSecondEntity);
    reconciled.chunkLoaded(overworld);
    expectGaugeCounts(reconciled, 1, 3, 1);

    WorldGaugeSnapshot repaired;
    repaired.actor_ids = {KPlayer, KSecondEntity, KSecondEntity};
    repaired.player_ids = {KPlayer, KPlayer};
    repaired.chunks = {nether_same_coordinates, nether_same_coordinates};
    reconciled.reconcile(repaired);
    expectGaugeCounts(reconciled, 1, 2, 1);

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
    expectGaugeCounts(from_scan, 1, 2, 2);

    from_events.playerRemoved(KPlayer);
    expectGaugeCounts(from_events, 0, 1, 2);
    from_events.playerSpawned(KPlayer);
    expectGaugeCounts(from_events, 1, 2, 2);
    from_events.reconcile(WorldGaugeSnapshot{});
    expectGaugeCounts(from_events, 0, 0, 0);
    from_events.actorRemoved(KEntity);
    from_events.playerRemoved(KPlayer);
    from_events.chunkUnloaded(overworld);
    expectGaugeCounts(from_events, 0, 0, 0);
}

}  // namespace

int main()
{
    testWorldGaugeLifecycle();

    FakePlugin owner;
    spark::StatisticsService statistics;
    spark::endstone_adapter::PapiIntegration integration;

    assert(integration.enable(owner, nullptr, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Unavailable);
    assert(!integration.registered());

    auto inactive = std::make_shared<FakePlaceholderApi>();
    inactive->active = false;
    assert(integration.enable(owner, inactive, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Unavailable);

    auto first = std::make_shared<FakePlaceholderApi>();
    assert(integration.enable(owner, first, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Registered);
    assert(integration.registered());
    assert(first->register_calls == 1);
    assert(first->expansion);
    assert(first->expansion->getIdentifier() == "spark");
    assert(first->expansion->getName() == "spark");
    assert(first->expansion->getAuthor() == "ReallocAll");
    assert(first->expansion->getVersion() == "0.6.0");
    assert(!first->expansion->supportsRelationalPlaceholders());
    assert(!first->expansion->supportsPlayerCleanup());
    assert(!first->expansion->onRequest(nullptr, "unknown").has_value());
    assert(first->setPlaceholders(nullptr, "{spark:unknown}") == "{spark:unknown}");

    assert(integration.enable(owner, first, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::AlreadyRegistered);
    assert(first->register_calls == 1);
    assert(integration.disable(owner));
    assert(!integration.registered());
    assert(first->unregister_calls == 1);
    assert(!first->expansion);

    auto rejected = std::make_shared<FakePlaceholderApi>();
    rejected->reject_registration = true;
    assert(integration.enable(owner, rejected, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Rejected);
    assert(!integration.registered());

    assert(integration.enable(owner, first, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Registered);
    first->active = false;
    assert(first->setPlaceholders(nullptr, "{spark:unknown}") == "{spark:unknown}");

    auto replacement = std::make_shared<FakePlaceholderApi>();
    assert(integration.enable(owner, replacement, statistics, "0.6.0") ==
           spark::endstone_adapter::PapiRegistrationResult::Registered);
    assert(first->unregister_calls == 1);
    assert(replacement->register_calls == 1);
    assert(integration.disable(owner));
    assert(replacement->unregister_calls == 1);
}
