#ifndef SPARK_CORE_UTIL_DEADLINE_THREAD_H
#define SPARK_CORE_UTIL_DEADLINE_THREAD_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>

#include <cerrno>
#include <thread>
#endif

namespace spark::detail {

class DeadlineThread final {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    DeadlineThread() noexcept = default;
    ~DeadlineThread() noexcept
    {
        if (joinable()) {
            std::terminate();
        }
    }
    DeadlineThread(const DeadlineThread &) = delete;
    DeadlineThread &operator=(const DeadlineThread &) = delete;

    bool start(std::function<void()> entry) noexcept
    {
        if (!entry) {
            return false;
        }
        try {
            auto run = std::make_shared<Run>(std::move(entry));
            {
                std::scoped_lock lock(mutex_);
                if (run_) {
                    return false;
                }
                run_ = run;
                failed_.store(false);
            }
#ifdef _WIN32
            unsigned int id = 0;
            const auto native_handle = _beginthreadex(nullptr, 0, &Run::trampoline, run.get(), 0, &id);
            // NOLINTNEXTLINE(performance-no-int-to-ptr): the CRT returns an integer handle.
            auto *const handle = reinterpret_cast<HANDLE>(native_handle);
            const bool created = handle != nullptr;
#else
            pthread_t thread{};
            const bool created = pthread_create(&thread, nullptr, &Run::trampoline, run.get()) == 0;
#endif
            if (!created) {
                run->entry = {};
                {
                    std::scoped_lock lock(mutex_);
                    if (run_ == run) {
                        run_.reset();
                    }
                }
                {
                    std::scoped_lock lock(run->mutex);
                    run->reaped = true;
                    run->published = true;
                }
                run->cv.notify_all();
                return false;
            }
            {
                std::scoped_lock lock(run->mutex);
#ifdef _WIN32
                run->handle = handle;
                run->id = id;
#else
                run->thread = thread;
#endif
                run->published = true;
            }
            run->cv.notify_all();
            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool joinable() const noexcept
    {
        std::scoped_lock lock(mutex_);
        return static_cast<bool>(run_);
    }

    bool isCurrentThread() const noexcept
    {
        std::scoped_lock lock(mutex_);
        return run_ && Run::mCurrent == run_.get();
    }

    bool failed() const noexcept
    {
        std::scoped_lock lock(mutex_);
        return failed_.load() || (run_ && run_->failed.load());
    }

    bool reapUntil(Deadline deadline) noexcept
    {
        std::shared_ptr<Run> run;
        {
            std::scoped_lock lock(mutex_);
            run = run_;
        }
        if (!run) {
            return true;
        }
        if (Run::mCurrent == run.get()) {
            return false;
        }
        try {
            std::unique_lock lock(run->mutex);
            if (!run->cv.wait_until(lock, deadline, [&] { return run->published && !run->reaping; })) {
                return false;
            }
            if (run->reaped) {
                return true;
            }
            run->reaping = true;
            lock.unlock();
            const bool exited = run->waitUntil(deadline);
            if (exited) {
                failed_.store(run->failed.load());
                run->entry = {};
                std::scoped_lock lifecycle_lock(mutex_);
                if (run_ == run) {
                    run_.reset();
                }
            }
            lock.lock();
            run->reaped = exited;
            run->reaping = false;
            lock.unlock();
            run->cv.notify_all();
            return exited;
        }
        catch (...) {
            std::scoped_lock lock(run->mutex);
            run->reaping = false;
            run->cv.notify_all();
            return false;
        }
    }

private:
    friend struct DeadlineThreadTestAccess;
    struct Run {
        explicit Run(std::function<void()> callback) : entry(std::move(callback)) {}
        std::mutex mutex;
        std::condition_variable cv;
        bool published = false;
        bool reaping = false;
        bool reaped = false;
        std::atomic<bool> failed{false};
        std::function<void()> entry;
        inline static thread_local Run *mCurrent = nullptr;
#ifdef _WIN32
        HANDLE handle = nullptr;
        unsigned int id = 0;
        static unsigned int __stdcall trampoline(void *argument)
        {
            static_cast<Run *>(argument)->execute();
            return 0;
        }
#else
        pthread_t thread{};
        static void *trampoline(void *argument)
        {
            static_cast<Run *>(argument)->execute();
            return nullptr;
        }
#endif
        void execute() noexcept
        {
            mCurrent = this;
            try {
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [this] { return published; });
                }
                entry();
            }
            catch (...) {
                failed.store(true);
            }
        }
        bool waitUntil(Deadline deadline) const noexcept
        {
            for (;;) {
#ifdef _WIN32
                const auto now = std::chrono::steady_clock::now();
                const auto remaining = now < deadline ? std::chrono::ceil<std::chrono::milliseconds>(deadline - now)
                                                      : std::chrono::milliseconds::zero();
                const auto timeout = static_cast<DWORD>(std::min<std::int64_t>(remaining.count(), INFINITE - 1));
                const auto result = WaitForSingleObject(handle, timeout);
                if (result == WAIT_OBJECT_0) {
                    return CloseHandle(handle) != 0;
                }
                if (result != WAIT_TIMEOUT || std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
#else
                const int result = pthread_tryjoin_np(thread, nullptr);
                if (result == 0) {
                    return true;
                }
                const auto now = std::chrono::steady_clock::now();
                if (result != EBUSY || now >= deadline) {
                    return false;
                }
                std::this_thread::sleep_until(std::min(deadline, now + std::chrono::milliseconds(1)));
#endif
            }
        }
    };
    mutable std::mutex mutex_;
    std::shared_ptr<Run> run_;
    std::atomic<bool> failed_{false};
};

}  // namespace spark::detail

#endif
