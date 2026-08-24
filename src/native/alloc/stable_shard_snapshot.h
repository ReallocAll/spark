#ifndef ENDSTONE_SPARK_STABLE_SHARD_SNAPSHOT_H
#define ENDSTONE_SPARK_STABLE_SHARD_SNAPSHOT_H

#include <chrono>
#include <cstddef>
#include <functional>
#include <utility>

namespace spark::detail {

template <typename UnlockShared>
class SharedShardLockGuard {
public:
    SharedShardLockGuard(UnlockShared &unlock_shared, std::size_t shard) noexcept
        : unlock_shared_(unlock_shared), shard_(shard)
    {
    }

    SharedShardLockGuard(const SharedShardLockGuard &) = delete;
    SharedShardLockGuard &operator=(const SharedShardLockGuard &) = delete;

    ~SharedShardLockGuard() noexcept { std::invoke(unlock_shared_, shard_); }

private:
    UnlockShared &unlock_shared_;
    std::size_t shard_;
};

// begin_attempt clears the staging buffer before each attempt and after timeout.
template <typename BeginAttempt, typename Version, typename TryLockShared, typename UnlockShared,
          typename ScanLockedShard, typename Now, typename Yield>
bool captureStableShardSnapshot(std::size_t shard_count, std::chrono::steady_clock::time_point deadline,
                                BeginAttempt &&begin_attempt, Version &&version, TryLockShared &&try_lock_shared,
                                UnlockShared &&unlock_shared, ScanLockedShard &&scan_locked_shard, Now &&now,
                                Yield &&yield)
{
    while (true) {
        if (std::invoke(now) >= deadline) {
            std::invoke(begin_attempt);
            return false;
        }

        const auto initial_version = std::invoke(version);
        std::invoke(begin_attempt);

        bool timed_out = false;
        for (std::size_t shard = 0; shard < shard_count; ++shard) {
            while (!std::invoke(try_lock_shared, shard)) {
                if (std::invoke(now) >= deadline) {
                    timed_out = true;
                    break;
                }
                std::invoke(yield);
            }
            if (timed_out) {
                break;
            }

            {
                SharedShardLockGuard<UnlockShared> lock_guard(unlock_shared, shard);
                std::invoke(scan_locked_shard, shard);
            }

            if (std::invoke(now) >= deadline) {
                timed_out = true;
                break;
            }
        }

        if (timed_out) {
            std::invoke(begin_attempt);
            return false;
        }
        if (std::invoke(version) == initial_version) {
            return true;
        }
        if (std::invoke(now) >= deadline) {
            std::invoke(begin_attempt);
            return false;
        }
    }
}

}  // namespace spark::detail

#endif
