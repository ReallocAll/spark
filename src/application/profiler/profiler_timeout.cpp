#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "application/profiler/profiler_timeout.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <utility>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>

#include <cerrno>
#include <thread>
#endif

namespace spark {

struct ProfilerTimeout::Run {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> cancelled{false};
    bool creation_done = false;
    bool native_owned = false;
    bool reaping = false;
    bool reaped = false;
    std::chrono::milliseconds delay;
    std::function<void()> callback;
#ifdef _WIN32
    HANDLE handle = nullptr;
    unsigned int thread_id = 0;
#else
    pthread_t thread{};
#endif
    inline static thread_local Run *mCurrent = nullptr;

    Run(std::chrono::milliseconds delay_value, std::function<void()> callback_value)
        : delay(delay_value), callback(std::move(callback_value))
    {
    }

    void execute() noexcept
    {
        mCurrent = this;
        try {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] { return creation_done; });
            const auto now = std::chrono::steady_clock::now();
            const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::time_point::max() - now);
            const auto deadline = delay >= maximum ? std::chrono::steady_clock::time_point::max() : now + delay;
            cv.wait_until(lock, deadline, [this] { return cancelled.load(std::memory_order_acquire); });
            if (cancelled.load(std::memory_order_acquire)) {
                return;
            }
            lock.unlock();
            callback();
        }
        catch (...) {  // NOLINT(bugprone-empty-catch): exceptions cannot escape the native worker.
        }
    }

#ifdef _WIN32
    static unsigned int __stdcall entry(void *argument)
    {
        static_cast<Run *>(argument)->execute();
        return 0;
    }
#else
    static void *entry(void *argument)
    {
        static_cast<Run *>(argument)->execute();
        return nullptr;
    }
#endif

    bool waitNativeUntil(std::chrono::steady_clock::time_point deadline) const noexcept
    {
#ifdef _WIN32
        for (;;) {
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
        }
#else
        for (;;) {
            const int result = pthread_tryjoin_np(thread, nullptr);
            if (result == 0) {
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (result != EBUSY || now >= deadline) {
                return false;
            }
            std::this_thread::sleep_until(std::min(deadline, now + std::chrono::milliseconds(1)));
        }
#endif
    }
};

ProfilerTimeout::~ProfilerTimeout()
{
    if (!cancelUntil(std::chrono::steady_clock::now() + std::chrono::seconds(5))) {
        std::terminate();
    }
}

bool ProfilerTimeout::arm(std::chrono::milliseconds delay, std::function<void()> callback) noexcept
{
    if (!callback || !cancelUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(500))) {
        return false;
    }
    try {
        auto run = std::make_shared<Run>(std::max(delay, std::chrono::milliseconds::zero()), std::move(callback));
        {
            std::scoped_lock lock(lifecycle_mutex_);
            if (run_) {
                return false;
            }
            run_ = run;
        }
#ifdef _WIN32
        unsigned int thread_id = 0;
        const auto native_handle = _beginthreadex(nullptr, 0, &Run::entry, run.get(), 0, &thread_id);
        // NOLINTNEXTLINE(performance-no-int-to-ptr): the CRT returns an integer handle.
        auto *const handle = reinterpret_cast<HANDLE>(native_handle);
        const bool created = handle != nullptr;
#else
        pthread_t thread{};
        const bool created = pthread_create(&thread, nullptr, &Run::entry, run.get()) == 0;
#endif
        {
            std::scoped_lock lock(run->mutex);
#ifdef _WIN32
            run->handle = handle;
            run->thread_id = thread_id;
#else
            run->thread = thread;
#endif
            run->native_owned = created;
            run->creation_done = true;
        }
        run->cv.notify_all();
        return created;
    }
    catch (...) {
        return false;
    }
}

void ProfilerTimeout::requestStop() noexcept
{
    std::shared_ptr<Run> run;
    {
        std::scoped_lock lock(lifecycle_mutex_);
        run = run_;
    }
    if (run) {
        {
            std::scoped_lock lock(run->mutex);
            run->cancelled.store(true, std::memory_order_release);
        }
        run->cv.notify_all();
    }
}

void ProfilerTimeout::cancel() noexcept
{
    static_cast<void>(cancelUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(500)));
}

bool ProfilerTimeout::cancelUntil(std::chrono::steady_clock::time_point deadline) noexcept
{
    requestStop();
    return reapUntil(deadline);
}

bool ProfilerTimeout::reapUntil(std::chrono::steady_clock::time_point deadline) noexcept
{
    std::shared_ptr<Run> run;
    {
        std::scoped_lock lock(lifecycle_mutex_);
        run = run_;
    }
    if (!run) {
        return true;
    }
    if (Run::mCurrent == run.get()) {
        return false;
    }
    bool stopped = false;
    try {
        std::unique_lock lock(run->mutex);
        if (!run->cv.wait_until(lock, deadline, [&run] { return run->creation_done && !run->reaping; })) {
            return false;
        }
        if (run->reaped) {
            return true;
        }
        run->reaping = true;
        const bool native_owned = run->native_owned;
        lock.unlock();
        stopped = !native_owned || run->waitNativeUntil(deadline);
        if (stopped) {
            std::scoped_lock lifecycle_lock(lifecycle_mutex_);
            if (run_ == run) {
                run_.reset();
            }
        }
        lock.lock();
        run->reaping = false;
        run->reaped = stopped;
        lock.unlock();
        run->cv.notify_all();
    }
    catch (...) {
        return false;
    }
    return stopped;
}

}  // namespace spark
