#ifndef SPARK_PLATFORM_LEVILAMINA_ADAPTERS_H
#define SPARK_PLATFORM_LEVILAMINA_ADAPTERS_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"
#include "core/stats/ping_statistics.h"

class CommandOrigin;
class CommandOutput;
class Player;

namespace ll::io {
class Logger;
}

namespace spark::levilamina {

class CallbackState;

// Shared monotonic start state anchored to the BDS process creation time.
class StartupClock final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    StartupClock() = default;

    StartupClock(StartupClock const&) = delete;
    StartupClock& operator=(StartupClock const&) = delete;

    bool initializeProcessStart();
    bool recordStart();
    [[nodiscard]] std::optional<TimePoint> startTime() const;

private:
    mutable std::mutex mutex_;
    std::optional<TimePoint> start_time_;
};

// Adapts CallbackState posting to the application main-thread capability.
class LeviLaminaDispatcher final : public MainThreadDispatcher {
public:
    explicit LeviLaminaDispatcher(std::shared_ptr<CallbackState> callback_state);

    void runOnMainThread(std::function<void()> task) override;

private:
    std::shared_ptr<CallbackState> callback_state_;
};

// Polls player ping from LeviLamina's main-thread Bedrock API.
class LeviLaminaPlayerPingProvider final : public PlayerPingProvider {
public:
    std::map<std::string, int> poll() override;
};

// Provides the server facts available through LeviLamina's typed Bedrock API.
class LeviLaminaMetadataProvider final : public ProfileMetadataProvider {
public:
    explicit LeviLaminaMetadataProvider(std::shared_ptr<const StartupClock> startup_clock);

    PlatformIdentity platformIdentity() const override;
    void gatherServerMetadata(ServerMetadata& metadata, std::int64_t now_ms) override;
    void gatherWorldMetadata(WorldInfo& world, std::string_view minecraft_version) override;
    std::vector<NativePluginSource> nativePluginSources() override;
    std::int64_t serverUptimeSeconds() override;
    std::int64_t playerCount() override;
    bool worldGaugesAvailable() override;
    WorldGaugeValues worldGauges() override;
    PlayerPingProvider* playerPingProvider() override;

private:
    [[nodiscard]] std::int64_t uptimeMilliseconds() const;

    std::shared_ptr<const StartupClock> startup_clock_;
    std::unique_ptr<LeviLaminaPlayerPingProvider> ping_provider_;
};

// Posts result delivery to the server thread and resolves the player afresh.
class LeviLaminaNotifier final : public ResultNotifier, public std::enable_shared_from_this<LeviLaminaNotifier> {
public:
    LeviLaminaNotifier(std::shared_ptr<CallbackState> callback_state, std::weak_ptr<ll::io::Logger> logger);

    void notify(const std::string& sender_name, const std::string& text) override;

private:
    void notifyOnMainThread(const std::string& sender_name, const std::string& text);

    std::shared_ptr<CallbackState> callback_state_;
    std::weak_ptr<ll::io::Logger> logger_;
};

// Synchronous, borrowed adapter for a single command callback invocation.
class BorrowedCommandSender final : public CommandSender {
public:
    BorrowedCommandSender(::CommandOrigin const& origin, ::CommandOutput& output);

    std::string getName() const override;
    bool isPlayer() const override;
    std::string getUniqueId() const override;
    bool hasPermission(const std::string& name) const override;

private:
    void sendImpl(const std::string& message) override;
    void errorImpl(const std::string& message) override;

    [[nodiscard]] ::Player const* resolvePlayer() const;

    ::CommandOrigin const& origin_;
    ::CommandOutput& output_;
};

using LevilaminaDispatcher = LeviLaminaDispatcher;
using LevilaminaMetadataProvider = LeviLaminaMetadataProvider;
using LevilaminaNotifier = LeviLaminaNotifier;

}  // namespace spark::levilamina

#endif  // SPARK_PLATFORM_LEVILAMINA_ADAPTERS_H
