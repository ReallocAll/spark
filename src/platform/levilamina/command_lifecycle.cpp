#include "platform/levilamina/command_lifecycle.h"

#include <utility>

namespace spark::levilamina {

void PublicationBoundary::begin() noexcept
{
    published_ = true;
}

bool PublicationBoundary::published() const noexcept
{
    return published_;
}

bool PublicationBoundary::requiresFailClosed() const noexcept
{
    return published_;
}

bool PublicationBoundary::confirmHostRegistryCleanup(bool registry_empty) noexcept
{
    if (!registry_empty) {
        return false;
    }
    published_ = false;
    return true;
}

struct CommandLifetimeGuard::Control {
    mutable std::mutex mutex;
    Phase phase = Phase::Open;
    std::optional<std::thread::id> server_thread;
    std::size_t active_commands = 0;
    bool unsafe_violation = false;
};

CommandLifetimeGuard::Lease::Lease(std::shared_ptr<Control> control) noexcept
    : control_(std::move(control)), held_(true)
{
}

CommandLifetimeGuard::Lease::Lease(Lease &&other) noexcept
    : control_(std::move(other.control_)), held_(std::exchange(other.held_, false))
{
}

CommandLifetimeGuard::Lease &CommandLifetimeGuard::Lease::operator=(Lease &&other) noexcept
{
    if (this == &other) {
        return *this;
    }
    reset();
    control_ = std::move(other.control_);
    held_ = std::exchange(other.held_, false);
    return *this;
}

CommandLifetimeGuard::Lease::~Lease() noexcept
{
    reset();
}

bool CommandLifetimeGuard::Lease::admitExecution() const noexcept
{
    if (!held_ || !control_) {
        return false;
    }

    std::lock_guard lock(control_->mutex);
    if (control_->phase != Phase::Open) {
        return false;
    }
    if (!control_->server_thread.has_value() || *control_->server_thread != std::this_thread::get_id()) {
        control_->unsafe_violation = true;
        return false;
    }
    return true;
}

void CommandLifetimeGuard::Lease::reset() noexcept
{
    if (!held_) {
        return;
    }
    held_ = false;
    if (!control_) {
        return;
    }

    std::lock_guard lock(control_->mutex);
    if (!control_->server_thread.has_value() || *control_->server_thread != std::this_thread::get_id() ||
        control_->active_commands == 0) {
        control_->unsafe_violation = true;
        return;
    }
    --control_->active_commands;
}

CommandLifetimeGuard::CommandLifetimeGuard() : control_(std::make_shared<Control>()) {}

CommandLifetimeGuard::~CommandLifetimeGuard() = default;

void CommandLifetimeGuard::observeServerThread(std::thread::id thread) noexcept
{
    std::lock_guard lock(control_->mutex);
    if (!control_->server_thread.has_value()) {
        control_->server_thread = thread;
    }
    else if (*control_->server_thread != thread) {
        control_->unsafe_violation = true;
    }
}

bool CommandLifetimeGuard::hasObservedServerThread() const noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->server_thread.has_value();
}

bool CommandLifetimeGuard::isServerThread() const noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->server_thread.has_value() && *control_->server_thread == std::this_thread::get_id();
}

std::optional<CommandLifetimeGuard::Lease> CommandLifetimeGuard::acquireForConstruction() noexcept
{
    const auto current_thread = std::this_thread::get_id();
    std::lock_guard lock(control_->mutex);
    if (control_->phase != Phase::Open || !control_->server_thread.has_value() ||
        *control_->server_thread != current_thread || control_->unsafe_violation) {
        return std::nullopt;
    }
    ++control_->active_commands;
    return Lease{control_};
}

bool CommandLifetimeGuard::beginCleanup() noexcept
{
    std::lock_guard lock(control_->mutex);
    if (control_->phase != Phase::Open || control_->active_commands != 0 || control_->unsafe_violation ||
        !control_->server_thread.has_value() || *control_->server_thread != std::this_thread::get_id()) {
        return false;
    }
    control_->phase = Phase::Closing;
    return true;
}

bool CommandLifetimeGuard::completeCleanup() noexcept
{
    std::lock_guard lock(control_->mutex);
    if (control_->phase != Phase::Closing || control_->active_commands != 0 || control_->unsafe_violation ||
        !control_->server_thread.has_value() || *control_->server_thread != std::this_thread::get_id()) {
        return false;
    }
    control_->phase = Phase::Closed;
    return true;
}

CommandLifetimeGuard::Phase CommandLifetimeGuard::phase() const noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->phase;
}

std::size_t CommandLifetimeGuard::activeCommands() const noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->active_commands;
}

bool CommandLifetimeGuard::unsafeViolation() const noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->unsafe_violation;
}

}  // namespace spark::levilamina
