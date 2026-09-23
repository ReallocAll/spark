#ifndef SPARK_PLATFORM_LEVILAMINA_WORLD_GAUGE_PROVIDER_H
#define SPARK_PLATFORM_LEVILAMINA_WORLD_GAUGE_PROVIDER_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "application/platform_capabilities.h"
#include "mc/deps/core/utility/pub_sub/Subscription.h"
#include "platform/endstone/world_gauge_event_adapter.h"
#include "platform/levilamina/bds/pubsub.h"
#include "platform/levilamina/bds/world_access.h"
#include "platform/levilamina/callback_state.h"

class ChunkSource;
class Dimension;
class Level;
class LevelChunk;

namespace spark::levilamina {

class CallbackState;
class WorldCallbackControl;

class ImportedSubscriptionSlot final {
public:
    ImportedSubscriptionSlot() = default;
    ImportedSubscriptionSlot(ImportedSubscriptionSlot const &) = delete;
    ImportedSubscriptionSlot &operator=(ImportedSubscriptionSlot const &) = delete;

    void assign(::Bedrock::PubSub::Subscription &&subscription)
    {
        if (connected_) {
            body_.disconnect();
        }
        bds::pubsub::moveSubscriptionBody(body_, subscription);
        connected_ = true;
    }

    void disconnect()
    {
        if (!connected_) {
            return;
        }
        body_.disconnect();
        connected_ = false;
    }

private:
    ::Bedrock::PubSub::SubscriptionBase body_;
    bool connected_ = false;
};

// Tracks host-admitted closures before they enter CallbackState.
class WorldCallbackAdmission final {
public:
    class Lease final {
    public:
        Lease() noexcept = default;
        explicit Lease(WorldCallbackAdmission *owner) noexcept : owner_(owner) {}
        Lease(Lease const &) = delete;
        Lease &operator=(Lease const &) = delete;
        Lease(Lease &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), previous_(std::exchange(other.previous_, nullptr))
        {
        }
        Lease &operator=(Lease &&other) noexcept
        {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
                previous_ = std::exchange(other.previous_, nullptr);
            }
            return *this;
        }
        ~Lease() { release(); }

        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
        void release() noexcept;

    private:
        friend class WorldCallbackAdmission;
        WorldCallbackAdmission *owner_ = nullptr;
        WorldCallbackAdmission *previous_ = nullptr;
    };

    WorldCallbackAdmission() = default;
    WorldCallbackAdmission(WorldCallbackAdmission const &) = delete;
    WorldCallbackAdmission &operator=(WorldCallbackAdmission const &) = delete;

    [[nodiscard]] Lease tryEnter() noexcept;
    void closeAdmission() noexcept;
    [[nodiscard]] bool waitQuiescent(std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool isActiveOnCurrentThread() const noexcept;
    [[nodiscard]] std::size_t activeCallbacks() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;

private:
    friend class Lease;
    void release() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool accepting_ = true;
    std::size_t active_callbacks_ = 0;
    static thread_local WorldCallbackAdmission *tls_owner_;
};

class LeviLaminaWorldGaugeProvider;

class WorldCallbackControl final : public std::enable_shared_from_this<WorldCallbackControl> {
public:
    class BodyLease final {
    public:
        BodyLease() noexcept = default;
        BodyLease(BodyLease const &) = delete;
        BodyLease &operator=(BodyLease const &) = delete;
        BodyLease(BodyLease &&other) noexcept;
        BodyLease &operator=(BodyLease &&other) noexcept;
        ~BodyLease() noexcept;

        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
        [[nodiscard]] LeviLaminaWorldGaugeProvider *provider() const noexcept { return provider_; }
        [[nodiscard]] std::shared_ptr<CallbackState> const &callbackState() const noexcept { return callback_state_; }

    private:
        friend class WorldCallbackControl;
        BodyLease(std::shared_ptr<WorldCallbackControl> owner, WorldCallbackAdmission::Lease admission,
                  LeviLaminaWorldGaugeProvider *provider, std::shared_ptr<CallbackState> callback_state) noexcept;

        void release() noexcept;

        std::shared_ptr<WorldCallbackControl> owner_;
        WorldCallbackAdmission::Lease admission_;
        LeviLaminaWorldGaugeProvider *provider_ = nullptr;
        std::shared_ptr<CallbackState> callback_state_;
    };

    WorldCallbackControl(LeviLaminaWorldGaugeProvider *provider, std::shared_ptr<CallbackState> callback_state);
    WorldCallbackControl(WorldCallbackControl const &) = delete;
    WorldCallbackControl &operator=(WorldCallbackControl const &) = delete;
    ~WorldCallbackControl() = default;

    [[nodiscard]] BodyLease enterBody() noexcept;
    void closeAdmission() noexcept;
    [[nodiscard]] bool waitQuiescent(std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool isActiveOnCurrentThread() const noexcept;
    void detachProvider() noexcept;

    void retainWrapper() noexcept;
    void releaseWrapper() noexcept;
    [[nodiscard]] std::size_t activeBodies() const noexcept;
    [[nodiscard]] std::size_t liveWrappers() const noexcept;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    LeviLaminaWorldGaugeProvider *provider_ = nullptr;
    std::shared_ptr<CallbackState> callback_state_;
    WorldCallbackAdmission admission_;
    bool accepting_ = true;
    std::size_t active_bodies_ = 0;
    std::size_t live_wrappers_ = 0;
};

class LeviLaminaWorldGaugeProvider final {
public:
    explicit LeviLaminaWorldGaugeProvider(std::shared_ptr<CallbackState> callback_state);
    ~LeviLaminaWorldGaugeProvider();

    LeviLaminaWorldGaugeProvider(LeviLaminaWorldGaugeProvider const &) = delete;
    LeviLaminaWorldGaugeProvider &operator=(LeviLaminaWorldGaugeProvider const &) = delete;

    // Call on the admitted server thread; subscriptions precede the first snapshot.
    [[nodiscard]] bool initialize(::Level &level);

    // Call after CallbackState stops admitting callbacks; disconnect is synchronous.
    [[nodiscard]] bool close(std::chrono::steady_clock::time_point deadline) noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool available() const noexcept { return initialized_ && available_; }
    [[nodiscard]] bool callbackClosuresQuiescent() const noexcept
    {
        return control_ != nullptr && control_->activeBodies() == 0 && control_->liveWrappers() == 0;
    }

    [[nodiscard]] WorldGaugeValues worldGauges();
    void gatherWorldMetadata(WorldInfo &world, std::string_view level_name_hint);

    // Called only by the signature-specific host callback wrappers after they
    // have acquired the shared admission/body lease.
    void onDimensionCreated(::Dimension &dimension) noexcept;
    void onChunkLoaded(::ChunkSource &, ::LevelChunk &chunk, int) noexcept;
    void onChunkDiscarded(::LevelChunk &chunk) noexcept;

private:
    [[nodiscard]] bool reconcile(std::string_view level_name_hint);
    void markUnavailable() noexcept;

    std::shared_ptr<CallbackState> callback_state_;
    std::shared_ptr<WorldCallbackControl> control_;
    std::unique_ptr<bds::WorldAccess> access_;
    endstone_adapter::EndstoneWorldGaugeEventAdapter event_adapter_;

    ImportedSubscriptionSlot dimension_subscription_;
    ImportedSubscriptionSlot chunk_loaded_subscription_;
    ImportedSubscriptionSlot chunk_discarded_subscription_;

    std::int64_t last_reconcile_steady_ms_ = 0;
    bool initialized_ = false;
    bool available_ = false;
    bool closed_ = false;
};

template <typename... Args>
class WorldCallbackFunction final {
public:
    using BodyFunction = void (*)(LeviLaminaWorldGaugeProvider &, Args...);

    WorldCallbackFunction() noexcept = default;
    WorldCallbackFunction(std::shared_ptr<WorldCallbackControl> control, BodyFunction function) noexcept
        : control_(std::move(control)), function_(function)
    {
        if (control_ != nullptr) {
            control_->retainWrapper();
        }
    }

    WorldCallbackFunction(WorldCallbackFunction const &other) noexcept
        : control_(other.control_), function_(other.function_)
    {
        if (control_ != nullptr) {
            control_->retainWrapper();
        }
    }

    WorldCallbackFunction(WorldCallbackFunction &&other) noexcept
        : control_(std::exchange(other.control_, nullptr)), function_(std::exchange(other.function_, nullptr))
    {
    }

    WorldCallbackFunction &operator=(WorldCallbackFunction const &other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        auto replacement_control = other.control_;
        if (replacement_control != nullptr) {
            replacement_control->retainWrapper();
        }
        auto replacement_function = other.function_;
        reset();
        control_ = std::move(replacement_control);
        function_ = replacement_function;
        return *this;
    }

    WorldCallbackFunction &operator=(WorldCallbackFunction &&other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        auto replacement_control = std::exchange(other.control_, nullptr);
        auto replacement_function = std::exchange(other.function_, nullptr);
        reset();
        control_ = std::move(replacement_control);
        function_ = replacement_function;
        return *this;
    }

    ~WorldCallbackFunction() { reset(); }

    void operator()(Args... args) const noexcept
    {
        if (control_ == nullptr || function_ == nullptr) {
            return;
        }
        auto body = control_->enterBody();
        if (!body) {
            return;
        }
        auto *provider = body.provider();
        auto callback_state = body.callbackState();
        if (provider == nullptr || callback_state == nullptr) {
            return;
        }
        auto function = function_;
        static_cast<void>(callback_state->invokeInline(
            [provider, function, &args...] { function(*provider, std::forward<Args>(args)...); }));
    }

private:
    void reset() noexcept
    {
        if (control_ != nullptr) {
            control_->releaseWrapper();
            control_.reset();
        }
        function_ = nullptr;
    }

    std::shared_ptr<WorldCallbackControl> control_;
    BodyFunction function_ = nullptr;
};

}  // namespace spark::levilamina

#endif  // SPARK_PLATFORM_LEVILAMINA_WORLD_GAUGE_PROVIDER_H
