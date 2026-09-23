#include "platform/levilamina/cleanup_deadline_guard.h"

#include <cstdlib>
#include <cstdint>
#include <exception>
#include <limits>

#ifdef _WIN32
#include <intrin.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace spark::levilamina {

struct CleanupDeadlineGuard::Control {
    enum class State {
        Dormant,
        Armed,
        Completed
    };

    mutable std::mutex mutex;
    std::condition_variable condition;
    State state = State::Dormant;
    Clock::time_point deadline{};
};

namespace {

constexpr auto kDormantCancellationTimeout = std::chrono::seconds{5};

[[noreturn]] void terminateOnTimeoutImpl() noexcept
{
#ifdef _WIN32
    if (::TerminateProcess(::GetCurrentProcess(), ERROR_TIMEOUT) == FALSE) {
        ::RaiseFailFastException(nullptr, nullptr, 0);
#if defined(_MSC_VER)
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
#endif
    }

    for (;;) {
    }
#else
    std::abort();
#endif
}

[[nodiscard]] bool waitForNativeThreadUntil(std::thread &thread,
                                            CleanupDeadlineGuard::Clock::time_point deadline) noexcept
{
#ifdef _WIN32
    const HANDLE native_handle = reinterpret_cast<HANDLE>(thread.native_handle());
    for (;;) {
        const auto now = CleanupDeadlineGuard::Clock::now();
        if (now >= deadline) {
            return false;
        }

        const auto remaining = deadline - now;
        auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (std::chrono::milliseconds{timeout.count()} < remaining) {
            ++timeout;
        }
        if (timeout <= std::chrono::milliseconds::zero()) {
            timeout = std::chrono::milliseconds{1};
        }

        const auto timeout_count = timeout.count();
        const auto wait_milliseconds =
            static_cast<DWORD>(timeout_count > static_cast<std::int64_t>((std::numeric_limits<DWORD>::max)() - 1)
                                   ? (std::numeric_limits<DWORD>::max)() - 1
                                   : timeout_count);
        const DWORD result = ::WaitForSingleObject(native_handle, wait_milliseconds);
        if (result == WAIT_OBJECT_0) {
            return CleanupDeadlineGuard::Clock::now() < deadline;
        }
        if (result != WAIT_TIMEOUT) {
            return false;
        }
    }
#else
    static_cast<void>(thread);
    static_cast<void>(deadline);
    return false;
#endif
}

}  // namespace

CleanupDeadlineGuard::CleanupDeadlineGuard() : control_(std::make_shared<Control>())
{
    auto control = control_;
    guard_thread_ = std::thread([control]() noexcept {
        try {
            std::unique_lock lock(control->mutex);
            control->condition.wait(lock, [&] { return control->state != Control::State::Dormant; });
            if (control->state == Control::State::Completed) {
                return;
            }

            const auto deadline = control->deadline;
            if (!control->condition.wait_until(lock, deadline,
                                               [&] { return control->state == Control::State::Completed; })) {
                lock.unlock();
                terminateOnTimeoutImpl();
            }
        }
        catch (...) {
            terminateOnTimeoutImpl();
        }
    });
}

CleanupDeadlineGuard::~CleanupDeadlineGuard()
{
    if (guard_thread_.joinable()) {
        std::terminate();
    }
}

void CleanupDeadlineGuard::arm(std::chrono::steady_clock::duration timeout)
{
    std::lock_guard lock(control_->mutex);
    if (control_->state != Control::State::Dormant) {
        return;
    }

    control_->deadline = Clock::now() + timeout;
    control_->state = Control::State::Armed;
    control_->condition.notify_all();
}

CleanupDeadlineGuard::Clock::time_point CleanupDeadlineGuard::deadline() const
{
    std::lock_guard lock(control_->mutex);
    return control_->deadline;
}

bool CleanupDeadlineGuard::cancelDormantAndJoin() noexcept
{
    std::lock_guard join_lock(join_mutex_);
    if (!guard_thread_.joinable()) {
        return true;
    }

    auto control = control_;
    {
        std::unique_lock lock(control->mutex);
        if (control->state == Control::State::Armed) {
            return false;
        }
        if (control->state == Control::State::Dormant) {
            control->state = Control::State::Completed;
            control->condition.notify_all();
        }
    }

    const auto deadline = Clock::now() + kDormantCancellationTimeout;
    if (!waitForNativeThreadUntil(guard_thread_, deadline)) {
        terminateOnTimeoutImpl();
    }

    try {
        guard_thread_.join();
    }
    catch (...) {
        terminateOnTimeoutImpl();
    }
    return true;
}

void CleanupDeadlineGuard::completeAndJoin()
{
    std::lock_guard join_lock(join_mutex_);
    if (!guard_thread_.joinable()) {
        return;
    }
    if (guard_thread_.get_id() == std::this_thread::get_id()) {
        terminateOnTimeoutImpl();
    }

    auto control = control_;
    Clock::time_point deadline;
    {
        std::unique_lock lock(control->mutex);
        if (control->state == Control::State::Dormant) {
            lock.unlock();
            terminateOnTimeoutImpl();
        }

        deadline = control->deadline;
        if (Clock::now() >= deadline) {
            lock.unlock();
            terminateOnTimeoutImpl();
        }
        control->state = Control::State::Completed;
        control->condition.notify_all();
    }

    if (!waitForNativeThreadUntil(guard_thread_, deadline)) {
        terminateOnTimeoutImpl();
    }

    try {
        guard_thread_.join();
    }
    catch (...) {
        terminateOnTimeoutImpl();
    }

    if (Clock::now() >= deadline) {
        terminateOnTimeoutImpl();
    }
}

[[noreturn]] void CleanupDeadlineGuard::terminateOnTimeout() noexcept
{
    terminateOnTimeoutImpl();
}

}  // namespace spark::levilamina
