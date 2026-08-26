#ifndef SPARK_APPLICATION_PROFILER_PROFILER_TIMEOUT_H
#define SPARK_APPLICATION_PROFILER_PROFILER_TIMEOUT_H

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace spark {

class ProfilerTimeout final {
public:
    ProfilerTimeout() = default;
    ~ProfilerTimeout();

    ProfilerTimeout(const ProfilerTimeout &) = delete;
    ProfilerTimeout &operator=(const ProfilerTimeout &) = delete;

    bool arm(std::chrono::milliseconds delay, std::function<void()> callback) noexcept;
    void cancel() noexcept;

private:
    struct State {
        std::condition_variable cv;
        std::mutex mutex;
        bool cancelled = false;
    };

    std::mutex lifecycle_mutex_;
    std::thread worker_;
    std::shared_ptr<State> state_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILER_TIMEOUT_H
