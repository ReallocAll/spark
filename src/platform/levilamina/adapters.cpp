#include "platform/levilamina/adapters.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "ll/api/Versions.h"
#include "ll/api/io/Logger.h"
#include "ll/api/service/Bedrock.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "platform/levilamina/callback_state.h"

namespace spark::levilamina {

bool StartupClock::recordStart()
{
    const auto now = Clock::now();
    std::lock_guard lock(mutex_);
    if (start_time_.has_value()) {
        return false;
    }
    start_time_ = now;
    return true;
}

std::optional<StartupClock::TimePoint> StartupClock::startTime() const
{
    std::lock_guard lock(mutex_);
    return start_time_;
}

LeviLaminaDispatcher::LeviLaminaDispatcher(std::shared_ptr<CallbackState> callback_state)
    : callback_state_(std::move(callback_state))
{
    if (!callback_state_) {
        throw std::invalid_argument{"LeviLamina dispatcher requires callback state"};
    }
}

void LeviLaminaDispatcher::runOnMainThread(std::function<void()> task)
{
    static_cast<void>(callback_state_->post(std::move(task)));
}

LeviLaminaMetadataProvider::LeviLaminaMetadataProvider(std::shared_ptr<const StartupClock> startup_clock)
    : startup_clock_(std::move(startup_clock))
{
    if (!startup_clock_ || !startup_clock_->startTime().has_value()) {
        throw std::invalid_argument{"LeviLamina metadata provider requires a captured server start time"};
    }
}

void LeviLaminaMetadataProvider::gatherServerMetadata(ServerMetadata& metadata, std::int64_t /*now_ms*/)
{
    metadata.endstone_version = ll::getLoaderVersion().to_string();
    metadata.minecraft_version = ll::getGameVersion().to_string();
    metadata.player_count = playerCount();
    metadata.online_mode = 0;
    metadata.uptime_ms = uptimeMilliseconds();
    metadata.plugins.clear();
    metadata.server_configurations.clear();
    metadata.platform_name = "LeviLamina";
    metadata.platform_brand = "LeviLamina";
}

void LeviLaminaMetadataProvider::gatherWorldMetadata(WorldInfo& world, std::string_view /*minecraft_version*/)
{
    world = WorldInfo{};
}

std::vector<NativePluginSource> LeviLaminaMetadataProvider::nativePluginSources()
{
    return {};
}

std::int64_t LeviLaminaMetadataProvider::serverUptimeSeconds()
{
    return uptimeMilliseconds() / 1000;
}

std::int64_t LeviLaminaMetadataProvider::playerCount()
{
    const auto level = ll::service::getLevel();
    if (!level) {
        return -1;
    }
    return static_cast<std::int64_t>(level->getActivePlayerCount());
}

bool LeviLaminaMetadataProvider::worldGaugesAvailable()
{
    return false;
}

WorldGaugeValues LeviLaminaMetadataProvider::worldGauges()
{
    return {};
}

PlayerPingProvider* LeviLaminaMetadataProvider::playerPingProvider()
{
    return nullptr;
}

std::int64_t LeviLaminaMetadataProvider::uptimeMilliseconds() const
{
    const auto start = startup_clock_->startTime();
    if (!start.has_value()) {
        throw std::logic_error{"LeviLamina server start time was not captured"};
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(StartupClock::Clock::now() - *start);
    return elapsed.count();
}

LeviLaminaNotifier::LeviLaminaNotifier(std::shared_ptr<CallbackState> callback_state,
                                       std::weak_ptr<ll::io::Logger> logger)
    : callback_state_(std::move(callback_state)), logger_(std::move(logger))
{
    if (!callback_state_) {
        throw std::invalid_argument{"LeviLamina notifier requires callback state"};
    }
}

void LeviLaminaNotifier::notify(const std::string& sender_name, const std::string& text)
{
    const auto weak_self = weak_from_this();
    static_cast<void>(callback_state_->post([weak_self, sender_name, text] {
        if (const auto self = weak_self.lock()) {
            self->notifyOnMainThread(sender_name, text);
        }
    }));
}

void LeviLaminaNotifier::notifyOnMainThread(const std::string& sender_name, const std::string& text)
{
    if (const auto logger = logger_.lock()) {
        logger->info("{}", text);
    }

    const auto level = ll::service::getLevel();
    if (!level) {
        return;
    }
    level->forEachPlayer([&](::Player& player) {
        if (player.getRealName() == sender_name) {
            player.sendMessage(text);
            return false;
        }
        return true;
    });
}

BorrowedCommandSender::BorrowedCommandSender(::CommandOrigin const& origin, ::CommandOutput& output)
    : origin_(origin), output_(output)
{
}

std::string BorrowedCommandSender::getName() const
{
    if (const auto* player = resolvePlayer()) {
        return player->getRealName();
    }
    return origin_.getName();
}

bool BorrowedCommandSender::isPlayer() const
{
    return resolvePlayer() != nullptr;
}

std::string BorrowedCommandSender::getUniqueId() const
{
    if (const auto* player = resolvePlayer()) {
        return player->getUuid().asString();
    }
    return {};
}

bool BorrowedCommandSender::hasPermission(const std::string& /*name*/) const
{
    return static_cast<int>(origin_.getPermissionsLevel())
        >= static_cast<int>(::CommandPermissionLevel::GameDirectors);
}

void BorrowedCommandSender::sendImpl(const std::string& message)
{
    output_.success(message);
}

void BorrowedCommandSender::errorImpl(const std::string& message)
{
    output_.error(message);
}

::Player const* BorrowedCommandSender::resolvePlayer() const
{
    auto* level = origin_.getLevel();
    auto* entity = origin_.getEntity();
    if (level == nullptr || entity == nullptr) {
        return nullptr;
    }

    ::Player const* result = nullptr;
    level->forEachPlayer([&](::Player& player) {
        if (static_cast<::Actor*>(&player) == entity) {
            result = &player;
            return false;
        }
        return true;
    });
    return result;
}

}  // namespace spark::levilamina
