#ifndef ENDSTONE_SPARK_NET_CANCELLATION_H
#define ENDSTONE_SPARK_NET_CANCELLATION_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace spark {

class CancellationToken {
public:
    CancellationToken() = default;

    [[nodiscard]] bool stopRequested() const noexcept
    {
        return state_ && state_->stopped.load(std::memory_order_acquire);
    }

    template <typename Rep, typename Period>
    bool waitForStop(std::chrono::duration<Rep, Period> timeout) const
    {
        if (!state_) {
            return false;
        }
        if (stopRequested()) {
            return true;
        }
        std::unique_lock lock(state_->mutex);
        return state_->cv.wait_for(lock, timeout,
                                   [state = state_] { return state->stopped.load(std::memory_order_acquire); });
    }

private:
    struct State {
        std::atomic<bool> stopped{false};
        std::mutex mutex;
        std::condition_variable cv;
    };

    explicit CancellationToken(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::shared_ptr<State> state_;

    friend class CancellationSource;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

    [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }

    void requestStop() noexcept
    {
        const auto state = state_;
        if (!state) {
            return;
        }
        state->stopped.store(true, std::memory_order_release);
        state->cv.notify_all();
    }

    void reset() { state_ = std::make_shared<CancellationToken::State>(); }

private:
    std::shared_ptr<CancellationToken::State> state_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_NET_CANCELLATION_H
