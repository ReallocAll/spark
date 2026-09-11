#ifndef SPARK_APPLICATION_PROFILER_PROFILER_TIMEOUT_H
#define SPARK_APPLICATION_PROFILER_PROFILER_TIMEOUT_H

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>

namespace spark {

class ProfilerTimeout final {
public:
    ProfilerTimeout() = default;
    ~ProfilerTimeout();

    ProfilerTimeout(const ProfilerTimeout &) = delete;
    ProfilerTimeout &operator=(const ProfilerTimeout &) = delete;

    bool arm(std::chrono::milliseconds delay, std::function<void()> callback) noexcept;
    void cancel() noexcept;
    void requestStop() noexcept;
    bool cancelUntil(std::chrono::steady_clock::time_point deadline) noexcept;
    bool reapUntil(std::chrono::steady_clock::time_point deadline) noexcept;

private:
    struct Run;
    std::mutex lifecycle_mutex_;
    std::shared_ptr<Run> run_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILER_TIMEOUT_H
