#include "ll/api/Global.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "application/command/command_sender.h"
#include "ll/api/Versions.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/GamingStatus.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandRawText.h"
#include "platform/levilamina/adapters.h"
#include "platform/levilamina/application_bridge.h"
#include "platform/levilamina/bds/tick_duration.h"
#include "platform/levilamina/callback_state.h"
#include "platform/levilamina/cleanup_deadline_guard.h"
#include "spark_constants.h"

struct SparkRawParameters {
    ::CommandRawText raw;
};

namespace {

using TickEvent = ll::event::ServerLevelTickEvent;
using ServerStartedEvent = ll::event::ServerStartedEvent;

template <typename Deleter>
class ProbeMemory final {
public:
    ProbeMemory(void* pointer, Deleter deleter) : pointer_(pointer), deleter_(deleter) {}
    ProbeMemory(ProbeMemory const&) = delete;
    ProbeMemory& operator=(ProbeMemory const&) = delete;
    ~ProbeMemory()
    {
        if (pointer_ != nullptr) {
            deleter_(pointer_);
        }
    }

    [[nodiscard]] void* get() const noexcept { return pointer_; }

private:
    void* pointer_;
    Deleter deleter_;
};

struct AlignedProbeDeleter {
    using DeleteFn = void (*)(void*, std::align_val_t) noexcept;

    DeleteFn function;
    std::align_val_t alignment;

    void operator()(void* pointer) const noexcept { function(pointer, alignment); }
};

struct HostSession final {
    std::unique_ptr<spark::levilamina::LeviLaminaDispatcher> dispatcher;
    std::unique_ptr<spark::levilamina::LeviLaminaMetadataProvider> metadata;
    std::shared_ptr<spark::levilamina::LeviLaminaNotifier> notifier;
    std::unique_ptr<spark::levilamina::ApplicationBridge> bridge;
    std::atomic_bool tick_started{false};
};

class SparkMod final {
public:
    bool load();
    bool enable();
    bool disable();

private:
    using Executor = ll::thread::ServerThreadExecutor;
    using CallbackState = spark::levilamina::CallbackState;
    using CleanupDeadlineGuard = spark::levilamina::CleanupDeadlineGuard;

    [[nodiscard]] ll::io::Logger& logger() const noexcept;
    [[nodiscard]] bool registerCommand();
    [[nodiscard]] bool runAllocatorProbe();
    [[nodiscard]] bool closeResources();
    void reportException(char const* operation, std::exception_ptr exception) const noexcept;

    std::weak_ptr<ll::mod::NativeMod> owner_;
    std::weak_ptr<ll::io::Logger> logger_;
    std::shared_ptr<CallbackState> callback_state_;
    std::unique_ptr<CleanupDeadlineGuard> cleanup_guard_;
    std::shared_ptr<spark::levilamina::StartupClock> startup_clock_;
    std::shared_ptr<HostSession> host_session_;
    std::shared_ptr<Executor> executor_;
    ll::event::ListenerPtr startup_listener_;
    ll::event::ListenerPtr tick_listener_;
    std::shared_ptr<std::atomic_bool> tick_warning_once_;
    std::atomic_bool enabled_{false};
    bool loaded_ = false;
    bool command_registered_ = false;
};

ll::io::Logger& SparkMod::logger() const noexcept
{
    if (auto logger = logger_.lock()) {
        return *logger;
    }
    std::terminate();
}

void SparkMod::reportException(char const* operation, std::exception_ptr exception) const noexcept
{
    auto logger = logger_.lock();
    if (!logger) {
        return;
    }

    try {
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
    catch (std::exception const& error) {
        try {
            logger->error("spark {} failed: {}", operation, error.what());
        }
        catch (...) {
        }
        return;
    }
    catch (...) {
        try {
            logger->error("spark {} failed with an unknown exception", operation);
        }
        catch (...) {
        }
        return;
    }
    try {
        logger->error("spark {} failed", operation);
    }
    catch (...) {
    }
}

bool SparkMod::runAllocatorProbe()
{
    using NewFn = void* (*)(std::size_t);
    using DeleteFn = void (*)(void*) noexcept;
    using AlignedNewFn = void* (*)(std::size_t, std::align_val_t);
    using AlignedDeleteFn = void (*)(void*, std::align_val_t) noexcept;

    const volatile NewFn scalar_new = static_cast<NewFn>(&::operator new);
    const volatile DeleteFn scalar_delete = static_cast<DeleteFn>(&::operator delete);
    ProbeMemory scalar{scalar_new(32), scalar_delete};
    if (scalar.get() == nullptr) {
        throw std::runtime_error{"scalar allocator returned null"};
    }
    auto* scalar_bytes = static_cast<volatile std::uint8_t*>(scalar.get());
    scalar_bytes[0] = 0x5A;
    if (scalar_bytes[0] != 0x5A) {
        throw std::runtime_error{"scalar allocator readback failed"};
    }

    const volatile NewFn array_new = static_cast<NewFn>(&::operator new[]);
    const volatile DeleteFn array_delete = static_cast<DeleteFn>(&::operator delete[]);
    ProbeMemory array{array_new(48), array_delete};
    if (array.get() == nullptr) {
        throw std::runtime_error{"array allocator returned null"};
    }
    auto* array_bytes = static_cast<volatile std::uint8_t*>(array.get());
    array_bytes[17] = 0xA5;
    if (array_bytes[17] != 0xA5) {
        throw std::runtime_error{"array allocator readback failed"};
    }

    constexpr std::align_val_t alignment{64};
    const volatile AlignedNewFn aligned_new = static_cast<AlignedNewFn>(&::operator new);
    const volatile AlignedDeleteFn aligned_delete = static_cast<AlignedDeleteFn>(&::operator delete);
    ProbeMemory aligned{
        aligned_new(96, alignment),
        AlignedProbeDeleter{aligned_delete, alignment},
    };
    if (aligned.get() == nullptr
        || reinterpret_cast<std::uintptr_t>(aligned.get()) % static_cast<std::size_t>(alignment) != 0) {
        throw std::runtime_error{"aligned allocator returned an invalid address"};
    }
    auto* aligned_bytes = static_cast<volatile std::uint8_t*>(aligned.get());
    aligned_bytes[31] = 0x3C;
    if (aligned_bytes[31] != 0x3C) {
        throw std::runtime_error{"aligned allocator readback failed"};
    }

    const volatile AlignedNewFn aligned_array_new = static_cast<AlignedNewFn>(&::operator new[]);
    const volatile AlignedDeleteFn aligned_array_delete = static_cast<AlignedDeleteFn>(&::operator delete[]);
    ProbeMemory aligned_array{
        aligned_array_new(128, alignment),
        AlignedProbeDeleter{aligned_array_delete, alignment},
    };
    if (aligned_array.get() == nullptr
        || reinterpret_cast<std::uintptr_t>(aligned_array.get()) % static_cast<std::size_t>(alignment) != 0) {
        throw std::runtime_error{"aligned array allocator returned an invalid address"};
    }
    auto* aligned_array_bytes = static_cast<volatile std::uint8_t*>(aligned_array.get());
    aligned_array_bytes[63] = 0xC3;
    if (aligned_array_bytes[63] != 0xC3) {
        throw std::runtime_error{"aligned array readback failed"};
    }

    logger().info("Spark allocator probe passed: scalar, array, aligned scalar, and aligned array paths");
    return true;
}

bool SparkMod::load()
{
    if (loaded_) {
        return true;
    }

    try {
        owner_ = ll::mod::NativeMod::current();
        auto owner = owner_.lock();
        if (!owner) {
            return false;
        }

        logger_ = owner->getLogger().weak_from_this();
        if (logger_.expired()) {
            logger_.reset();
            return false;
        }

        std::filesystem::create_directories(owner->getDataDir());
        std::filesystem::create_directories(owner->getConfigDir());

        cleanup_guard_ = std::make_unique<CleanupDeadlineGuard>();
        callback_state_ = std::make_shared<CallbackState>();
        startup_clock_ = std::make_shared<spark::levilamina::StartupClock>();
        tick_warning_once_ = std::make_shared<std::atomic_bool>(false);

        const auto weak_logger = logger_;
        auto state = callback_state_;
        state->setInfoCallback([weak_logger](std::string const& message) {
            if (auto logger = weak_logger.lock()) {
                logger->info("{}", message);
            }
        });
        state->setErrorCallback([weak_logger](std::string const& message) {
            if (auto logger = weak_logger.lock()) {
                logger->error("{}", message);
            }
        });
        state->setFatalHandler([](CallbackState::FatalReason) { CleanupDeadlineGuard::terminateOnTimeout(); });

        const std::weak_ptr<spark::levilamina::StartupClock> weak_clock = startup_clock_;
        startup_listener_ = ll::event::EventBus::getInstance().emplaceListener<ServerStartedEvent>(
            [state, weak_clock](ServerStartedEvent&) {
                static_cast<void>(state->invokeInline([weak_clock] {
                    if (auto clock = weak_clock.lock()) {
                        static_cast<void>(clock->recordStart());
                    }
                }));
            },
            ll::event::EventPriority::Normal,
            owner_
        );
        if (!startup_listener_) {
            static_cast<void>(closeResources());
            logger().error("spark load failed: could not register ServerStartedEvent listener");
            return false;
        }

        loaded_ = true;
        logger().info("Spark {} loaded for LeviLamina {}", spark::kVersion, ll::getLoaderVersion().to_string());
        logger().info("Spark data directory: {}", owner->getDataDir().string());
        logger().info("Spark config directory: {}", owner->getConfigDir().string());
        return true;
    }
    catch (...) {
        auto exception = std::current_exception();
        static_cast<void>(closeResources());
        reportException("load", exception);
        return false;
    }
}

bool SparkMod::registerCommand()
{
    auto registry = ll::service::getCommandRegistry();
    if (!registry) {
        throw std::runtime_error{"spark command registration failed: server command registry is unavailable"};
    }

    if (auto* existing = registry->findCommand("spark"); existing != nullptr) {
        if (existing->permissionLevel != ::CommandPermissionLevel::GameDirectors || !existing->overloads.empty()) {
            throw std::runtime_error{"spark command registration rejected a conflicting pre-existing signature"};
        }
    }

    auto& registrar = ll::command::CommandRegistrar::getServerInstance();
    auto state = callback_state_;
    const std::weak_ptr<HostSession> weak_session = host_session_;
    auto& command = registrar.getOrCreateCommand(
        "spark", "Spark profiler", ::CommandPermissionLevel::GameDirectors, ::CommandFlagValue::NotCheat, owner_
    );

    const auto dispatch = [state, weak_session](
                              ::CommandOrigin const& origin,
                              ::CommandOutput& output,
                              std::string raw_text
                          ) {
        bool handled = false;
        const bool admitted = state->invokeInline([&] {
            auto session = weak_session.lock();
            if (!session || !session->bridge) {
                return;
            }
            spark::levilamina::BorrowedCommandSender sender(origin, output);
            handled = session->bridge->dispatch(sender, raw_text);
        });
        if (!admitted) {
            output.error("Spark is disabled");
        }
        else if (!handled) {
            output.error("Spark command was not handled");
        }
    };

    command.overload(owner_).execute([dispatch](::CommandOrigin const& origin, ::CommandOutput& output) {
        dispatch(origin, output, {});
    });
    command.overload<SparkRawParameters>(owner_).required("raw").execute(
        [dispatch](::CommandOrigin const& origin, ::CommandOutput& output, SparkRawParameters const& params) {
            dispatch(origin, output, params.raw.getText());
        }
    );
    command_registered_ = true;
    logger().info("Registered /spark with GameDirectors permission");
    return true;
}

bool SparkMod::enable()
{
    try {
        if (enabled_.load(std::memory_order_acquire)) {
            return true;
        }
        if (!loaded_ || !cleanup_guard_ || !callback_state_ || !startup_clock_) {
            throw std::runtime_error{"spark enable requires a completed load"};
        }

        auto owner = owner_.lock();
        if (!owner) {
            throw std::runtime_error{"spark enable could not resolve its owning native mod"};
        }
        if (callback_state_->phase() != CallbackState::Phase::Open) {
            throw std::runtime_error{"spark enable requires an open callback state"};
        }
        if (!startup_clock_->startTime().has_value()) {
            throw std::runtime_error{"spark enable requires ServerStartedEvent startup timing"};
        }

        if (!runAllocatorProbe()) {
            throw std::runtime_error{"spark allocator probe failed"};
        }

        auto state = callback_state_;
        const auto weak_logger = logger_;
        executor_ = std::make_shared<Executor>("spark_server_thread", std::chrono::milliseconds{30}, 16);
        const std::weak_ptr<Executor> weak_executor = executor_;
        state->setSubmitter([state, weak_executor](std::function<void()> wrapper) {
            auto executor = weak_executor.lock();
            if (!executor) {
                throw std::runtime_error{"server executor is unavailable"};
            }
            executor->execute(std::move(wrapper));
        });

        auto session = std::make_shared<HostSession>();
        host_session_ = session;
        session->dispatcher = std::make_unique<spark::levilamina::LeviLaminaDispatcher>(state);
        session->metadata = std::make_unique<spark::levilamina::LeviLaminaMetadataProvider>(startup_clock_);
        session->notifier = std::make_shared<spark::levilamina::LeviLaminaNotifier>(state, weak_logger);
        session->bridge = std::make_unique<spark::levilamina::ApplicationBridge>(
            owner->getDataDir(), owner->getConfigDir(), *session->dispatcher, *session->metadata, *session->notifier
        );
        session->bridge->enable();

        const std::weak_ptr<HostSession> weak_session = session;
        const auto warning_once = tick_warning_once_;
        tick_listener_ = ll::event::EventBus::getInstance().emplaceListener<TickEvent>(
            [state, weak_session, warning_once](TickEvent&) {
                static_cast<void>(state->invokeInline([state, weak_session, warning_once] {
                    state->observeTick();
                    const auto measured_ms = spark::levilamina::bds::readServerTickMilliseconds();
                    auto session = weak_session.lock();
                    if (!session) {
                        return;
                    }

                    const bool first_event = !session->tick_started.exchange(true, std::memory_order_acq_rel);
                    if (!measured_ms.has_value()) {
                        if (warning_once && !warning_once->exchange(true, std::memory_order_acq_rel)) {
                            state->reportError("Spark skipped an invalid BDS server tick duration");
                        }
                    }
                    else if (!first_event && session->bridge) {
                        session->bridge->onTick(*measured_ms, spark::levilamina::bds::currentThreadId());
                    }
                }));
            },
            ll::event::EventPriority::Normal,
            owner_
        );
        if (!tick_listener_) {
            throw std::runtime_error{"spark enable failed: could not register ServerLevelTickEvent listener"};
        }

        // Keep command registration last so every earlier resource can unwind on failure.
        if (!registerCommand()) {
            throw std::runtime_error{"spark command registration failed"};
        }

        enabled_.store(true, std::memory_order_release);
        logger().info("Spark {} enabled; SparkApplication forwarding is active", spark::kVersion);
        return true;
    }
    catch (...) {
        auto exception = std::current_exception();
        static_cast<void>(closeResources());
        reportException("enable", exception);
        return false;
    }
}

bool SparkMod::closeResources()
{
    try {
        auto state = callback_state_;
        if (!state) {
            if (!cleanup_guard_) {
                enabled_.store(false, std::memory_order_release);
                startup_clock_.reset();
                tick_warning_once_.reset();
                return true;
            }
            if (!cleanup_guard_->cancelDormantAndJoin()) {
                CleanupDeadlineGuard::terminateOnTimeout();
            }
            cleanup_guard_.reset();
            startup_clock_.reset();
            tick_warning_once_.reset();
            enabled_.store(false, std::memory_order_release);
            return true;
        }

        constexpr auto cleanup_timeout = std::chrono::seconds{5};
        if (!cleanup_guard_) {
            state->failFatal(CallbackState::FatalReason::Deadline);
            CleanupDeadlineGuard::terminateOnTimeout();
        }
        cleanup_guard_->arm(cleanup_timeout);
        const auto deadline = cleanup_guard_->deadline();

        std::shared_ptr<Executor> executor;
        ll::event::ListenerPtr startup_listener;
        ll::event::ListenerPtr tick_listener;
        const auto claim = state->beginClosing([&] {
            startup_listener = std::move(startup_listener_);
            tick_listener = std::move(tick_listener_);
            executor = std::move(executor_);
        });

        if (claim == CallbackState::CloseClaim::AlreadyClosed) {
            enabled_.store(false, std::memory_order_release);
            return true;
        }
        if (claim == CallbackState::CloseClaim::AlreadyClosing) {
            if (state->waitClosed(deadline)) {
                enabled_.store(false, std::memory_order_release);
                return true;
            }
            state->failFatal(CallbackState::FatalReason::Deadline);
            CleanupDeadlineGuard::terminateOnTimeout();
        }
        if (claim == CallbackState::CloseClaim::SelfWaitRejected) {
            state->failFatal(CallbackState::FatalReason::SelfWait);
            CleanupDeadlineGuard::terminateOnTimeout();
        }

        auto cleanup_scope = state->enterCleanupScope();
        if (!state->waitProducer(deadline)) {
            state->failFatal(CallbackState::FatalReason::Deadline);
            CleanupDeadlineGuard::terminateOnTimeout();
        }

        auto pending_payloads = state->takePendingPayloads();
        state->destroyPendingPayloads(pending_payloads);

        if (startup_listener) {
            ll::event::EventBus::getInstance().removeListener<ServerStartedEvent>(startup_listener);
        }
        if (tick_listener) {
            ll::event::EventBus::getInstance().removeListener<TickEvent>(tick_listener);
        }
        startup_listener.reset();
        tick_listener.reset();

        auto diagnostic_callbacks = state->takeDiagnosticCallbacks();
        state->destroyDiagnosticCallbacks(diagnostic_callbacks);

        if (!state->waitQuiescent(deadline)) {
            state->failFatal(CallbackState::FatalReason::Deadline);
            CleanupDeadlineGuard::terminateOnTimeout();
        }

        auto session = std::move(host_session_);
        if (session && session->bridge) {
            std::string error;
            if (!session->bridge->shutdown(error)) {
                state->failFatal(CallbackState::FatalReason::Deadline);
                CleanupDeadlineGuard::terminateOnTimeout();
            }
            session->bridge.reset();
        }
        if (session) {
            session->notifier.reset();
            session->metadata.reset();
            session->dispatcher.reset();
        }
        session.reset();
        executor.reset();

        startup_clock_.reset();
        tick_warning_once_.reset();

        const auto active_bodies = state->activeBodies();
        const auto pending_work_slots = state->pendingWorkSlots();
        const bool startup_listener_released = startup_listener == nullptr;
        const bool tick_listener_released = tick_listener == nullptr;
        const bool executor_released = executor == nullptr;
        if (active_bodies != 0 || pending_work_slots != 0 || !startup_listener_released || !tick_listener_released
            || !executor_released) {
            state->failFatal(CallbackState::FatalReason::Deadline);
            CleanupDeadlineGuard::terminateOnTimeout();
        }

        const auto ticks = state->rawTickObservations();
        logger().info(
            "Spark cleanup quiescent: ticks={}, active_bodies={}, pending_payloads={}, "
            "startup_listener_released={}, tick_listener_released={}, executor_released={}",
            ticks,
            active_bodies,
            pending_work_slots,
            startup_listener_released,
            tick_listener_released,
            executor_released
        );
        enabled_.store(false, std::memory_order_release);
        logger().info("Spark disabled after {} admitted ServerLevelTickEvent callbacks", ticks);
        cleanup_guard_->completeAndJoin();
        state->markClosed();
        return true;
    }
    catch (...) {
        CleanupDeadlineGuard::terminateOnTimeout();
    }
}

bool SparkMod::disable()
{
    if (ll::getGamingStatus() == ll::GamingStatus::Running) {
        logger().warn("Spark disable refused while LeviLamina gaming status is Running; restart is required");
        return false;
    }

    try {
        return closeResources();
    }
    catch (...) {
        CleanupDeadlineGuard::terminateOnTimeout();
    }
}

SparkMod& getSparkMod()
{
    static SparkMod mod;
    return mod;
}

}  // namespace

LL_REGISTER_MOD(SparkMod, getSparkMod());
