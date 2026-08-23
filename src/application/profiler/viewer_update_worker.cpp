#include "application/profiler/viewer_update_worker.h"

#include <utility>

namespace spark {

ViewerUpdateWorker::ViewerUpdateWorker(ExecuteCallback execute, CompletionCallback completion)
    : execute_(std::move(execute)), completion_(std::move(completion))
{
}

ViewerUpdateWorker::~ViewerUpdateWorker()
{
    stop();
}

bool ViewerUpdateWorker::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    {
        std::scoped_lock lock(mutex_);
        work_.reset();
        work_active_ = false;
        open_pending_ = false;
        running_.store(true, std::memory_order_release);
    }
    try {
        thread_ = std::thread([this] { run(); });
    }
    catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void ViewerUpdateWorker::stop()
{
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
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
        WorkItem work;
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

bool ViewerUpdateWorker::enqueueUpdate(ExportContext context, std::shared_ptr<ViewerSocket> socket,
                                       std::uint64_t generation)
{
    {
        std::scoped_lock lock(mutex_);
        if (!running_.load(std::memory_order_acquire) || work_ || work_active_ || generation != generation_) {
            return false;
        }
        WorkItem work;
        work.type = WorkType::Update;
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
    return !work_ && !work_active_;
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
    try {
        while (running_.load(std::memory_order_acquire)) {
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

            {
                std::scoped_lock lock(mutex_);
                work_active_ = false;
            }

            if (work.type == WorkType::Open) {
                Completion completion;
                completion.type = work.type;
                completion.generation = work.generation;
                completion.url = std::move(url);
                completion.socket = std::move(work.socket);
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
        work_active_ = false;
        open_pending_ = false;
        work_.reset();
    }
    catch (...) {
        failed_.store(true, std::memory_order_release);
    }
}

}  // namespace spark
