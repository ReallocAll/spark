#ifndef ENDSTONE_SPARK_LINUX_OWNED_THREAD_H
#define ENDSTONE_SPARK_LINUX_OWNED_THREAD_H

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdlib>

namespace spark {

class LinuxOwnedThread {
public:
    LinuxOwnedThread() = default;
    ~LinuxOwnedThread()
    {
        if (joinable()) {
            std::abort();
        }
    }
    LinuxOwnedThread(const LinuxOwnedThread &) = delete;
    LinuxOwnedThread &operator=(const LinuxOwnedThread &) = delete;

    bool create(void *(*entry)(void *), void *context) noexcept
    {
        if (joinable() || ::pthread_create(&thread_, nullptr, entry, context) != 0) {
            return false;
        }
        owned_.store(true, std::memory_order_release);
        return true;
    }
    bool joinable() const noexcept { return owned_.load(std::memory_order_acquire); }
    bool joinUntil(std::chrono::steady_clock::time_point deadline) noexcept
    {
        while (joinable()) {
            if (::pthread_tryjoin_np(thread_, nullptr) == 0) {
                owned_.store(false, std::memory_order_release);
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            const timespec delay{0, 1000000};
            ::nanosleep(&delay, nullptr);
        }
        return true;
    }

private:
    pthread_t thread_{};
    std::atomic<bool> owned_{false};
};

}  // namespace spark

#endif
