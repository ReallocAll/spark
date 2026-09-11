#ifndef SPARK_APPLICATION_PROFILER_VIEWER_UPDATE_WORKER_H
#define SPARK_APPLICATION_PROFILER_VIEWER_UPDATE_WORKER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/profiler/profiler.h"
#include "core/util/deadline_thread.h"
#include "core/ws/viewer_socket.h"

namespace spark {

// Runs live-viewer open and update work away from the server thread.
class ViewerUpdateWorker {
public:
    enum class WorkType {
        Open,
        Statistics,
        Sampler,
        Combined
    };

    struct WorkItem {
        WorkType type = WorkType::Combined;
        ExportContext context;
        std::shared_ptr<ViewerSocket> socket;
        std::uint64_t generation = 0;
        std::string sender_name;
        CancellationToken cancellation;
    };

    struct Completion {
        WorkType type = WorkType::Combined;
        std::uint64_t generation = 0;
        std::string url;
        std::weak_ptr<ViewerSocket> socket;
        std::string sender_name;
    };

    using ExecuteCallback = std::function<std::string(const WorkItem &)>;
    using CompletionCallback = std::function<void(Completion)>;

    ViewerUpdateWorker(ExecuteCallback execute, CompletionCallback completion);
    ~ViewerUpdateWorker();

    ViewerUpdateWorker(const ViewerUpdateWorker &) = delete;
    ViewerUpdateWorker &operator=(const ViewerUpdateWorker &) = delete;

    bool start();
    void stop();
    void requestStop();
    bool stopUntil(std::chrono::steady_clock::time_point deadline);
    bool quiesceUntil(std::chrono::steady_clock::time_point deadline);

    std::optional<std::uint64_t> enqueueOpen(ExportContext context, std::shared_ptr<ViewerSocket> socket,
                                             std::string sender_name);
    bool enqueueStatistics(ExportContext context, std::shared_ptr<ViewerSocket> socket, std::uint64_t generation);
    bool enqueueSampler(ExportContext context, std::shared_ptr<ViewerSocket> socket, std::uint64_t generation);
    bool enqueueCombined(ExportContext context, std::shared_ptr<ViewerSocket> socket, std::uint64_t generation);

    void invalidate();
    bool current(std::uint64_t generation) const;
    bool available() const;
    bool openPending() const;
    std::uint64_t generation() const;
    bool consumeFailure() { return failed_.exchange(false, std::memory_order_acq_rel); }
    bool completeOpen(std::uint64_t generation);

private:
    bool enqueueWork(WorkType type, ExportContext context, std::shared_ptr<ViewerSocket> socket,
                     std::uint64_t generation);
    void run() noexcept;
    void markFailure() noexcept;

    ExecuteCallback execute_;
    CompletionCallback completion_;

    detail::DeadlineThread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable exit_cv_;
    bool exited_ = true;
    CancellationSource cancellation_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::optional<WorkItem> work_;
    bool work_active_ = false;
    bool open_pending_ = false;
    std::uint64_t generation_ = 0;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_VIEWER_UPDATE_WORKER_H
