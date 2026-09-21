#include "platform/levilamina/world_gauge_provider.h"

#include <stdexcept>
#include <utility>

namespace spark::levilamina {

WorldCallbackControl::BodyLease::BodyLease(std::shared_ptr<WorldCallbackControl> owner,
                                           WorldCallbackAdmission::Lease admission,
                                           LeviLaminaWorldGaugeProvider *provider,
                                           std::shared_ptr<CallbackState> callback_state) noexcept
    : owner_(std::move(owner)), admission_(std::move(admission)), provider_(provider),
      callback_state_(std::move(callback_state))
{
}

WorldCallbackControl::BodyLease::BodyLease(BodyLease &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), admission_(std::move(other.admission_)),
      provider_(std::exchange(other.provider_, nullptr)), callback_state_(std::move(other.callback_state_))
{
}

WorldCallbackControl::BodyLease &WorldCallbackControl::BodyLease::operator=(BodyLease &&other) noexcept
{
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        admission_ = std::move(other.admission_);
        provider_ = std::exchange(other.provider_, nullptr);
        callback_state_ = std::move(other.callback_state_);
    }
    return *this;
}

WorldCallbackControl::BodyLease::~BodyLease() noexcept
{
    release();
}

void WorldCallbackControl::BodyLease::release() noexcept
{
    auto owner = std::exchange(owner_, nullptr);
    if (owner == nullptr) {
        return;
    }
    {
        std::lock_guard lock(owner->mutex_);
        if (owner->active_bodies_ != 0) {
            --owner->active_bodies_;
        }
        owner->condition_.notify_all();
    }
    admission_.release();
    provider_ = nullptr;
    callback_state_.reset();
}

WorldCallbackControl::WorldCallbackControl(LeviLaminaWorldGaugeProvider *provider,
                                           std::shared_ptr<CallbackState> callback_state)
    : provider_(provider), callback_state_(std::move(callback_state))
{
    if (callback_state_ == nullptr) {
        throw std::invalid_argument{"LeviLamina world callback control requires callback state"};
    }
}

WorldCallbackControl::BodyLease WorldCallbackControl::enterBody() noexcept
{
    std::lock_guard lock(mutex_);
    if (!accepting_ || provider_ == nullptr || callback_state_ == nullptr) {
        return {};
    }
    auto admission = admission_.tryEnter();
    if (!admission) {
        return {};
    }

    std::shared_ptr<WorldCallbackControl> owner;
    try {
        owner = shared_from_this();
    }
    catch (...) {
        admission.release();
        return {};
    }
    ++active_bodies_;
    return BodyLease{std::move(owner), std::move(admission), provider_, callback_state_};
}

void WorldCallbackControl::closeAdmission() noexcept
{
    std::lock_guard lock(mutex_);
    accepting_ = false;
    admission_.closeAdmission();
    condition_.notify_all();
}

bool WorldCallbackControl::waitQuiescent(std::chrono::steady_clock::time_point deadline) noexcept
{
    if (isActiveOnCurrentThread()) {
        return false;
    }
    {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_until(lock, deadline, [this] { return active_bodies_ == 0 && live_wrappers_ == 0; })) {
            return false;
        }
    }
    return admission_.waitQuiescent(deadline);
}

bool WorldCallbackControl::isActiveOnCurrentThread() const noexcept
{
    return admission_.isActiveOnCurrentThread();
}

void WorldCallbackControl::detachProvider() noexcept
{
    std::lock_guard lock(mutex_);
    provider_ = nullptr;
    callback_state_.reset();
    condition_.notify_all();
}

void WorldCallbackControl::retainWrapper() noexcept
{
    std::lock_guard lock(mutex_);
    ++live_wrappers_;
    condition_.notify_all();
}

void WorldCallbackControl::releaseWrapper() noexcept
{
    std::lock_guard lock(mutex_);
    if (live_wrappers_ != 0) {
        --live_wrappers_;
    }
    condition_.notify_all();
}

std::size_t WorldCallbackControl::activeBodies() const noexcept
{
    std::lock_guard lock(mutex_);
    return active_bodies_;
}

std::size_t WorldCallbackControl::liveWrappers() const noexcept
{
    std::lock_guard lock(mutex_);
    return live_wrappers_;
}

thread_local WorldCallbackAdmission *WorldCallbackAdmission::tls_owner_ = nullptr;

void WorldCallbackAdmission::Lease::release() noexcept
{
    auto *owner = std::exchange(owner_, nullptr);
    if (owner == nullptr) {
        return;
    }
    owner->release();
    WorldCallbackAdmission::tls_owner_ = std::exchange(previous_, nullptr);
}

WorldCallbackAdmission::Lease WorldCallbackAdmission::tryEnter() noexcept
{
    std::lock_guard lock(mutex_);
    if (!accepting_) {
        return {};
    }
    ++active_callbacks_;
    Lease lease{this};
    lease.previous_ = tls_owner_;
    tls_owner_ = this;
    return lease;
}

void WorldCallbackAdmission::closeAdmission() noexcept
{
    std::lock_guard lock(mutex_);
    accepting_ = false;
    condition_.notify_all();
}

bool WorldCallbackAdmission::waitQuiescent(std::chrono::steady_clock::time_point deadline) noexcept
{
    if (isActiveOnCurrentThread()) {
        return false;
    }
    std::unique_lock lock(mutex_);
    return condition_.wait_until(lock, deadline, [this] { return active_callbacks_ == 0; });
}

bool WorldCallbackAdmission::isActiveOnCurrentThread() const noexcept
{
    return tls_owner_ == this;
}

std::size_t WorldCallbackAdmission::activeCallbacks() const noexcept
{
    std::lock_guard lock(mutex_);
    return active_callbacks_;
}

bool WorldCallbackAdmission::accepting() const noexcept
{
    std::lock_guard lock(mutex_);
    return accepting_;
}

void WorldCallbackAdmission::release() noexcept
{
    std::lock_guard lock(mutex_);
    if (active_callbacks_ != 0) {
        --active_callbacks_;
    }
    condition_.notify_all();
}

}  // namespace spark::levilamina
