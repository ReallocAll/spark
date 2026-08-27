from pathlib import Path

path = Path("src/native/alloc/allocation_sampler_linux.cpp")
text = path.read_text()


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, got {count}: {old[:180]!r}")
    text = text.replace(old, new, 1)


replace_once(
    "constexpr std::size_t KHookCallShards = 1024;\n"
    "constexpr std::size_t KMaxSampledThreads = 256;",
    "constexpr std::size_t KHookCallShards = 1024;\n"
    "constexpr std::size_t KLiveLockAttempts = 64;\n"
    "constexpr std::size_t KMaxSampledThreads = 256;",
)

replace_once(
    """    bool shouldTrackCurrentThread() const noexcept
    {
        if (!tracking.load(std::memory_order_relaxed)) {
            return false;
        }
        if (config.count_only) {
            return true;
        }
        auto *self = const_cast<Impl *>(this);
        ThreadSamplingState *state = self->currentThreadState();
        return state != nullptr && !state->tracking_suppressed;
    }

    static std::uint64_t liveIndexHash(void *pointer) noexcept
""",
    """    bool shouldTrackCurrentThread() const noexcept
    {
        if (!tracking.load(std::memory_order_relaxed)) {
            return false;
        }
        if (config.count_only) {
            return true;
        }
        auto *self = const_cast<Impl *>(this);
        ThreadSamplingState *state = self->currentThreadState();
        return state != nullptr && !state->tracking_suppressed;
    }

    static void liveLockPause() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    bool tryLockLivePool() noexcept
    {
        if (config.force_live_lock_contention_for_testing) {
            return false;
        }
        for (std::size_t attempt = 0; attempt < KLiveLockAttempts; ++attempt) {
            if (::pthread_mutex_trylock(&live_pool_mutex) == 0) {
                return true;
            }
            liveLockPause();
        }
        return false;
    }

    bool tryLockLiveIndexShard(std::size_t shard) noexcept
    {
        if (config.force_live_lock_contention_for_testing) {
            return false;
        }
        for (std::size_t attempt = 0; attempt < KLiveLockAttempts; ++attempt) {
            if (::pthread_rwlock_trywrlock(&live_index_locks[shard]) == 0) {
                return true;
            }
            liveLockPause();
        }
        return false;
    }

    static std::uint64_t liveIndexHash(void *pointer) noexcept
""",
)

replace_once(
    """        const std::size_t shard = liveIndexShard(hash);
        if (config.force_live_lock_contention_for_testing ||
            ::pthread_rwlock_trywrlock(&live_index_locks[shard]) != 0) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
""",
    """        const std::size_t shard = liveIndexShard(hash);
        if (!tryLockLiveIndexShard(shard)) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
""",
)

replace_once(
    """    LiveAllocation *acquireLiveRecord() noexcept
    {
        if (config.force_live_lock_contention_for_testing || ::pthread_mutex_trylock(&live_pool_mutex) != 0) {
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
""",
    """    LiveAllocation *acquireLiveRecord() noexcept
    {
        if (!tryLockLivePool()) {
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
""",
)

replace_once(
    """        const std::size_t presence_slot = livePresenceSlot(hash);
        const std::size_t shard = liveIndexShard(hash);
        if (config.force_live_lock_contention_for_testing ||
            ::pthread_rwlock_trywrlock(&live_index_locks[shard]) != 0) {
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
""",
    """        const std::size_t presence_slot = livePresenceSlot(hash);
        const std::size_t shard = liveIndexShard(hash);
        if (!tryLockLiveIndexShard(shard)) {
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
""",
)

replace_once(
    """        if (config.force_live_lock_contention_for_testing || ::pthread_mutex_trylock(&live_pool_mutex) != 0) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
""",
    """        if (!tryLockLivePool()) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
""",
)

path.write_text(text)
