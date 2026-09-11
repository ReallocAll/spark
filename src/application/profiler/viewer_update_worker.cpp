#include "application/profiler/viewer_update_worker.h"

#include <exception>
#include <utility>

namespace spark {

ViewerUpdateWorker::ViewerUpdateWorker(ExecuteCallback execute, CompletionCallback completion)
    : execute_(std::move(execute)), completion_(std::move(completion))
{
}

ViewerUpdateWorker::~ViewerUpdateWorker()
{
    if (!stopUntil(std::chrono::steady_clock::now() + std::chrono::seconds(5))) {
        std::terminate();
    }
}

bool ViewerUpdateWorker::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!thread_.reapUntil(std::chrono::steady_clock::now())) {
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        work_.reset();
        work_active_ = false;
        open_pending_ = false;
        cancellation_.reset();
        exited_ = false;
        running_.store(true, std::memory_order_release);
    }
    try {
        if (!thread_.start([this] { run(); })) {
            running_.store(false, std::memory_order_release);
            std::scoped_lock lock(mutex_);
            exited_ = true;
            return false;
        }
    }
    catch (...) {
        running_.store(false, std::memory_order_release);
        std::scoped_lock lock(mutex_);
        exited_ = true;
        return false;
    }
    return true;
}

void ViewerUpdateWorker::stop()
{
    static_cast<void>(stopUntil(std::chrono::steady_clock::now() + std::chrono::seconds(5)));
}

void ViewerUpdateWorker::requestStop()
{
    {
        std::scoped_lock lock(mutex_);
        cancellation_.requestStop();
        work_.reset();
        open_pending_ = false;
    }
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
}

bool ViewerUpdateWorker::stopUntil(std::chrono::steady_clock::time_point deadline)
{
    requestStop();
    return thread_.reapUntil(deadline);
}

bool ViewerUpdateWorker::quiesceUntil(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(mutex_);
    if (!exit_cv_.wait_until(lock, deadline, [this] { return !work_ && !work_active_; })) {
        return false;
    }
    const bool running = running_.load(std::memory_order_acquire);
    lock.unlock();
    return running || thread_.reapUntil(deadline);
}

std::optional<std::uint64_t> ViewerUpdateWorker::enqueueOpen(ExportContext context,
                                                             std::shared_ptr<ViewerSocket> socket,
                                                             std::string sender_name)
{
    std::optional<std::uint64_t> result;
    {
        std::scoped_lock lock(mutex_);
        if (!running_.load(std::memory_order_acquire) || work_ || work_active_ || open_pending_) {
            return std::nullopt;
        }
        ++generation_;
        cancellation_.reset();
        WorkItem work;
        work.cancellation = cancellation_.token();
        work.type = WorkType::Open;
        work.context = std::move(context);
        work.socket = std::move(socket);
        work.generation = generation_;
        work.sender_name = std::move(sender_name);
        work_ = std::move(work);
        open_pending_ = true;
        result = generation_;
    }
    cv_.notify_one();
    return result;
}

bool ViewerUpdateWorker::enqueueStatistics(ExportContext context, std::shared_ptr<ViewerSocket> socket,
                                           std::uint64_t generation)
{
    return enqueueWork(WorkType::Statistics, std::move(context), std::move(socket), generation);
}

bool ViewerUpdateWorker::enqueueSampler(ExportContext context, std::shared_ptr<ViewerSocket> socket,
                                        std::uint64_t generation)
{
    return enqueueWork(WorkType::Sampler, std::move(context), std::move(socket), generation);
}

bool ViewerUpdateWorker::enqueueCombined(ExportContext context, std::shared_ptr<ViewerSocket> socket,
                                         std::uint64_t generation)
{
    return enqueueWork(WorkType::Combined, std::move(context), std::move(socket), generation);
}

bool ViewerUpdateWorker::enqueueWork(WorkType type, ExportContext context, std::shared_ptr<ViewerSocket> socket,
                                     std::uint64_t generation)
{
    {
        std::scoped_lock lock(mutex_);
        if (!running_.load(std::memory_order_acquire) || work_ || work_active_ || generation != generation_) {
            return false;
        }
        WorkItem work;
        work.cancellation = cancellation_.token();
        work.type = type;
        work.context = std::move(context);
        work.socket = std::move(socket);
        work.generation = generation;
        work_ = std::move(work);
    }
    cv_.notify_one();
    return true;
}

void ViewerUpdateWorker::invalidate()
{
    std::scoped_lock lock(mutex_);
    cancellation_.requestStop();
    ++generation_;
    work_.reset();
    open_pending_ = false;
}

bool ViewerUpdateWorker::current(std::uint64_t generation) const
{
    std::scoped_lock lock(mutex_);
    return running_.load(std::memory_order_acquire) && generation == generation_;
}

bool ViewerUpdateWorker::available() const
{
    std::scoped_lock lock(mutex_);
    return !work_ && !work_active_ && (running_.load(std::memory_order_acquire) || !thread_.joinable());
}

bool ViewerUpdateWorker::openPending() const
{
    std::scoped_lock lock(mutex_);
    return open_pending_;
}

std::uint64_t ViewerUpdateWorker::generation() const
{
    std::scoped_lock lock(mutex_);
    return generation_;
}

bool ViewerUpdateWorker::completeOpen(std::uint64_t generation)
{
    std::scoped_lock lock(mutex_);
    if (generation != generation_) {
        return false;
    }
    open_pending_ = false;
    return true;
}

void ViewerUpdateWorker::run() noexcept
{
    struct ExitGuard {
        ViewerUpdateWorker &worker;
        ~ExitGuard()
        {
            {
                std::scoped_lock lock(worker.mutex_);
                worker.work_active_ = false;
                worker.exited_ = true;
            }
            worker.exit_cv_.notify_all();
        }
    } exit_guard{*this};
    try {
        while (running_.load(std::memory_order_acquire)) {
            {
                WorkItem work;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [this] { return !running_.load(std::memory_order_acquire) || work_.has_value(); });
                    if (!running_.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (!work_) {
                        continue;
                    }
                    work = std::move(*work_);
                    work_.reset();
                    work_active_ = true;
                }

                std::string url;
                try {
                    url = execute_(work);
                }
                catch (...) {
                    markFailure();
                    return;
                }

                if (work.type == WorkType::Open) {
                    Completion completion;
                    completion.type = work.type;
                    completion.generation = work.generation;
                    completion.url = std::move(url);
                    completion.socket = work.socket;
                    completion.sender_name = std::move(work.sender_name);
                    try {
                        completion_(std::move(completion));
                    }
                    catch (...) {
                        markFailure();
                        return;
                    }
                }
            }
            {
                std::scoped_lock lock(mutex_);
                work_active_ = false;
            }
            exit_cv_.notify_all();
        }
    }
    catch (...) {
        markFailure();
    }
}

void ViewerUpdateWorker::markFailure() noexcept
{
    running_.store(false, std::memory_order_release);
    failed_.store(true, std::memory_order_release);
    try {
        std::scoped_lock lock(mutex_);
        cancellation_.requestStop();
        open_pending_ = false;
        work_.reset();
    }
    catch (...) {
        failed_.store(true, std::memory_order_release);
    }
}

}  // namespace spark
