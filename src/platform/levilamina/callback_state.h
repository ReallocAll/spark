#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spark::levilamina {

class CallbackState final : public std::enable_shared_from_this<CallbackState> {
    struct TlsFrame {
        CallbackState *state = nullptr;
        TlsFrame *previous = nullptr;
    };

public:
    enum class Phase {
        Open,
        Closing,
        Closed
    };
    enum class CloseClaim {
        Owner,
        AlreadyClosing,
        AlreadyClosed,
        SelfWaitRejected
    };
    enum class FatalReason {
        Deadline,
        SelfWait
    };

    using Submitter = std::function<void(std::function<void()>)>;
    using Diagnostic = std::function<void(std::string const &)>;
    using FatalHandler = std::function<void(FatalReason)>;

    struct DiagnosticCallbacks {
        std::shared_ptr<Diagnostic> info;
        std::shared_ptr<Diagnostic> error;

        DiagnosticCallbacks() = default;
        DiagnosticCallbacks(DiagnosticCallbacks const &) = delete;
        DiagnosticCallbacks &operator=(DiagnosticCallbacks const &) = delete;
        DiagnosticCallbacks(DiagnosticCallbacks &&other) noexcept;
        DiagnosticCallbacks &operator=(DiagnosticCallbacks &&other) noexcept;
        ~DiagnosticCallbacks() noexcept;

    private:
        friend class CallbackState;
        std::shared_ptr<CallbackState> owner_;
        std::size_t reserved_activity_ = 0;
    };

    class CleanupScope {
    public:
        CleanupScope() = default;

        CleanupScope(CleanupScope const &) = delete;
        CleanupScope &operator=(CleanupScope const &) = delete;
        CleanupScope(CleanupScope &&) = delete;
        CleanupScope &operator=(CleanupScope &&) = delete;
        ~CleanupScope() noexcept;

    private:
        friend class CallbackState;
        explicit CleanupScope(CallbackState *state) noexcept;

        CallbackState *state_ = nullptr;
        TlsFrame frame_{};
    };

    CallbackState() = default;

    CallbackState(CallbackState const &) = delete;
    CallbackState &operator=(CallbackState const &) = delete;

    void setSubmitter(Submitter submitter);
    void setInfoCallback(Diagnostic callback);
    void setErrorCallback(Diagnostic callback);
    void setFatalHandler(FatalHandler handler);

    [[nodiscard]] bool post(std::function<void()> body);
    [[nodiscard]] bool invokeInline(std::function<void()> body);
    [[nodiscard]] CloseClaim beginClosing();
    [[nodiscard]] CloseClaim beginClosing(std::function<void()> detach_resources);

    [[nodiscard]] CleanupScope enterCleanupScope() noexcept;

    [[nodiscard]] std::vector<std::function<void()>> takePendingPayloads();
    void destroyPendingPayloads(std::vector<std::function<void()>> &payloads);
    void releasePendingPayloads(std::size_t count);
    [[nodiscard]] DiagnosticCallbacks takeDiagnosticCallbacks();
    void destroyDiagnosticCallbacks(DiagnosticCallbacks &callbacks) noexcept;
    [[nodiscard]] bool waitProducer(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool waitQuiescent(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool waitClosed(std::chrono::steady_clock::time_point deadline);

    void reserveProducer();
    void setProducerThreadIdentity();
    void rollbackProducerReservation();
    void markProducerDone();
    void observeTick();
    void markClosed();
    void reportInfo(std::string message) noexcept;
    void reportError(std::string message) noexcept;
    void reportException(std::exception_ptr exception) noexcept;
    void failFatal(FatalReason reason);

    [[nodiscard]] bool producerStopRequested() const;
    [[nodiscard]] bool isProducerThread() const;
    [[nodiscard]] bool isInBodyOnCurrentThread() const;
    [[nodiscard]] Phase phase() const;
    [[nodiscard]] std::uint64_t rawTickObservations() const;
    [[nodiscard]] std::uint64_t activeBodies() const;
    [[nodiscard]] std::uint64_t pendingWorkSlots() const;

private:
    friend struct CallbackStateTestAccess;

    struct WorkSlot;

    class ActivityScope {
    public:
        ActivityScope() = delete;
        ActivityScope(ActivityScope const &) = delete;
        ActivityScope &operator=(ActivityScope const &) = delete;
        ActivityScope(ActivityScope &&) = delete;
        ActivityScope &operator=(ActivityScope &&) = delete;
        ~ActivityScope() noexcept;

        void release() noexcept;

    private:
        friend class CallbackState;
        ActivityScope(CallbackState *state, std::size_t count) noexcept;

        CallbackState *state_;
        TlsFrame frame_{};
        std::size_t count_;
        bool released_ = false;
    };

    void invokeSlot(std::shared_ptr<WorkSlot> const &slot);
    void removePendingSlotLocked(std::shared_ptr<WorkSlot> const &slot) noexcept;
    [[nodiscard]] bool cancelSlotLocked(std::shared_ptr<WorkSlot> const &slot, std::function<void()> &payload);
    [[nodiscard]] ActivityScope adoptActive(std::size_t count) noexcept;
    void releaseActive(std::size_t count) noexcept;
    void reserveActiveLocked(std::size_t count) noexcept;
    void destroySubmitter(std::shared_ptr<Submitter> &submitter, bool reserved) noexcept;
    void destroyDiagnostic(std::shared_ptr<Diagnostic> &callback, bool reserved) noexcept;
    void destroyFatalHandler(std::shared_ptr<FatalHandler> &handler, bool reserved) noexcept;
    void report(std::shared_ptr<Diagnostic> callback, std::string message) noexcept;
    void pushTls(TlsFrame &frame) noexcept;
    void popTls(TlsFrame &frame) noexcept;
    [[nodiscard]] bool isActiveOnCurrentThread() const noexcept;
    static TlsFrame *&tlsTop() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    Phase phase_ = Phase::Open;
    bool producer_stop_requested_ = false;
    bool producer_started_ = false;
    bool producer_done_ = true;
    std::thread::id producer_thread_;
    std::uint64_t active_bodies_ = 0;
    std::uint64_t pending_work_slots_ = 0;
    std::uint64_t raw_tick_observations_ = 0;
    std::vector<std::shared_ptr<WorkSlot>> pending_;
    std::shared_ptr<Submitter> submitter_;
    std::shared_ptr<Diagnostic> info_callback_;
    std::shared_ptr<Diagnostic> error_callback_;
    std::shared_ptr<Diagnostic> detached_info_callback_;
    std::shared_ptr<Diagnostic> detached_error_callback_;
    std::size_t detached_diagnostic_activity_ = 0;
    std::shared_ptr<FatalHandler> fatal_handler_;
};

}  // namespace spark::levilamina
