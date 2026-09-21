#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace spark::levilamina {

class PublicationBoundary final {
public:
    PublicationBoundary() = default;

    void begin() noexcept;
    [[nodiscard]] bool published() const noexcept;
    [[nodiscard]] bool requiresFailClosed() const noexcept;
    [[nodiscard]] bool confirmHostRegistryCleanup(bool registry_empty) noexcept;

private:
    bool published_ = false;
};

template <typename CleanupFn, typename ReportFn, typename FailClosedFn>
[[nodiscard]] bool runPublicationFailurePath(
    PublicationBoundary const &boundary,
    std::exception_ptr exception,
    CleanupFn &&cleanup,
    ReportFn &&report,
    FailClosedFn &&fail_closed
)
{
    const auto safe_report = [&] {
        try {
            report(exception);
        }
        catch (...) {
        }
    };

    if (boundary.requiresFailClosed()) {
        safe_report();
        fail_closed();
        return false;
    }

    try {
        cleanup();
    }
    catch (...) {
        fail_closed();
        return false;
    }
    safe_report();
    return false;
}

class CommandLifetimeGuard final {
    struct Control;

public:
    enum class Phase {
        Open,
        Closing,
        Closed
    };

    class Lease final {
    public:
        Lease() = default;
        Lease(Lease const &) = delete;
        Lease &operator=(Lease const &) = delete;

        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        ~Lease() noexcept;

        [[nodiscard]] bool admitExecution() const noexcept;
        void reset() noexcept;

    private:
        friend class CommandLifetimeGuard;
        explicit Lease(std::shared_ptr<Control> control) noexcept;

        std::shared_ptr<Control> control_;
        bool held_ = false;
    };

    CommandLifetimeGuard();
    ~CommandLifetimeGuard();

    CommandLifetimeGuard(CommandLifetimeGuard const &) = delete;
    CommandLifetimeGuard &operator=(CommandLifetimeGuard const &) = delete;

    void observeServerThread(std::thread::id thread) noexcept;
    [[nodiscard]] bool hasObservedServerThread() const noexcept;
    [[nodiscard]] bool isServerThread() const noexcept;

    [[nodiscard]] std::optional<Lease> acquireForConstruction() noexcept;
    [[nodiscard]] bool beginCleanup() noexcept;
    [[nodiscard]] bool completeCleanup() noexcept;

    [[nodiscard]] Phase phase() const noexcept;
    [[nodiscard]] std::size_t activeCommands() const noexcept;
    [[nodiscard]] bool unsafeViolation() const noexcept;

private:
    std::shared_ptr<Control> control_;
};

template <typename T>
[[nodiscard]] bool waitForSoleSharedOwner(std::shared_ptr<T> const &value,
                                          std::chrono::steady_clock::time_point deadline) noexcept
{
    while (value && value.use_count() > 1) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

}  // namespace spark::levilamina
