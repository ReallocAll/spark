#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace spark::levilamina {

class CleanupDeadlineGuard final {
public:
    using Clock = std::chrono::steady_clock;

    CleanupDeadlineGuard();
    ~CleanupDeadlineGuard();

    CleanupDeadlineGuard(CleanupDeadlineGuard const&)            = delete;
    CleanupDeadlineGuard& operator=(CleanupDeadlineGuard const&) = delete;

    void arm(std::chrono::steady_clock::duration timeout);
    [[nodiscard]] Clock::time_point deadline() const;
    void completeAndJoin();
    [[nodiscard]] bool cancelDormantAndJoin() noexcept;

    [[noreturn]] static void terminateOnTimeout() noexcept;

private:
    struct Control;

    std::shared_ptr<Control> control_;
    std::mutex join_mutex_;
    std::thread guard_thread_;
};

}  // namespace spark::levilamina
