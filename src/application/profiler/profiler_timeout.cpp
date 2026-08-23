#include "application/profiler/profiler_timeout.h"

#include <utility>

namespace spark {

ProfilerTimeout::~ProfilerTimeout()
{
    cancel();
}

bool ProfilerTimeout::arm(std::chrono::milliseconds delay, std::function<void()> callback) noexcept
{
    if (!callback) {
        return false;
    }

    cancel();

    if (delay < std::chrono::milliseconds::zero()) {
        delay = std::chrono::milliseconds::zero();
    }

    std::shared_ptr<State> state;
    try {
        state = std::make_shared<State>();
        std::scoped_lock lock(lifecycle_mutex_);
        state_ = state;
        worker_ = std::thread([state, delay, callback = std::move(callback)]() mutable noexcept {
            try {
                std::unique_lock lock(state->mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto maximum_delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::time_point::max() - now);
                const auto deadline =
                    delay >= maximum_delay ? std::chrono::steady_clock::time_point::max() : now + delay;
                state->cv.wait_until(lock, deadline, [&state] { return state->cancelled; });
                if (state->cancelled) {
                    return;
                }
                lock.unlock();
                try {
                    callback();
                }
                catch (...) {  // NOLINT(bugprone-empty-catch): callbacks cannot escape the worker thread.
                }
            }
            catch (...) {  // NOLINT(bugprone-empty-catch): worker exceptions cannot escape the thread.
            }
        });
    }
    catch (...) {
        std::scoped_lock lock(lifecycle_mutex_);
        if (state_ == state) {
            state_.reset();
        }
        return false;
    }
    return true;
}

void ProfilerTimeout::cancel() noexcept
{
    std::thread worker;
    try {
        {
            std::scoped_lock lock(lifecycle_mutex_);
            const auto state = state_;
            if (state) {
                {
                    std::scoped_lock state_lock(state->mutex);
                    state->cancelled = true;
                }
                state->cv.notify_all();
            }

            if (!worker_.joinable()) {
                state_.reset();
                return;
            }
            if (worker_.get_id() == std::this_thread::get_id()) {
                worker_.detach();
                state_.reset();
                return;
            }
            worker = std::move(worker_);
            state_.reset();
        }
        worker.join();
    }
    catch (...) {
        if (worker.joinable()) {
            worker.detach();
        }
    }
}

}  // namespace spark
