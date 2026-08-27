from pathlib import Path

path = Path("src/native/alloc/allocation_sampler_linux.cpp")
text = path.read_text()


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, got {count}: {old[:160]!r}")
    text = text.replace(old, new, 1)


# Fast TLS lookup: pthread TLS already identifies the calling thread once the
# registry slot is fully published, so avoid gettid on every allocator hook.
start_marker = "    ThreadSamplingState *currentThreadState() noexcept\n"
end_marker = "\n    bool shouldTrackCurrentThread() const noexcept"
start = text.index(start_marker)
end = text.index(end_marker, start)
replacement = """    ThreadSamplingState *currentThreadState() noexcept
    {
        if (!thread_state_key_created) {
            return nullptr;
        }
        void *value = ::pthread_getspecific(thread_state_key);
        if (value == tombstonePointer()) {
            return nullptr;
        }
        auto *state = static_cast<ThreadSamplingState *>(value);
        const auto address = reinterpret_cast<std::uintptr_t>(state);
        const auto begin = reinterpret_cast<std::uintptr_t>(thread_states.data());
        const std::size_t state_limit =
            config.thread_state_limit_for_testing == 0
                ? thread_states.size()
                : (std::min)(thread_states.size(), static_cast<std::size_t>(config.thread_state_limit_for_testing));
        const std::uintptr_t end = begin + state_limit * sizeof(ThreadSamplingState);

        // pthread TLS is already scoped to the calling thread. Once a slot is
        // fully published, re-reading gettid on every allocator hook is
        // redundant and very expensive on the Linux hot path.
        if (address >= begin && address < end && state->registry_state.load(std::memory_order_acquire) == 2) {
            return state;
        }

        const auto tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
        if (address >= begin && address < end && state->registry_state.load(std::memory_order_acquire) != 0 &&
            state->owner_tid.load(std::memory_order_acquire) == tid) {
            return state;
        }

        for (std::size_t i = 0; i < state_limit; ++i) {
            ThreadSamplingState &candidate = thread_states[i];
            if (candidate.registry_state.load(std::memory_order_acquire) == 1 &&
                candidate.owner_tid.load(std::memory_order_acquire) == tid) {
                return &candidate;
            }
        }

        ThreadSamplingState *claimed = nullptr;
        for (std::size_t i = 0; i < state_limit; ++i) {
            ThreadSamplingState &candidate = thread_states[i];
            std::uint8_t expected = 0;
            if (candidate.registry_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                claimed = &candidate;
                break;
            }
        }
        if (claimed == nullptr) {
            thread_state_drops.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        claimed->owner_tid.store(tid, std::memory_order_release);
        claimed->bytes = {};
        claimed->identity_generation = 0;
        claimed->session_thread_id = 0;
        claimed->os_thread_id = tid;
        claimed->inside_hook = true;
        claimed->tracking_suppressed = false;
        claimed->identity_announced = false;
        if (::pthread_setspecific(thread_state_key, claimed) != 0) {
            claimed->inside_hook = false;
            claimed->owner_tid.store(0, std::memory_order_relaxed);
            claimed->registry_state.store(0, std::memory_order_release);
            thread_state_drops.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        claimed->inside_hook = false;
        claimed->registry_state.store(2, std::memory_order_release);
        return claimed;
    }
"""
text = text[:start] + replacement + text[end:]

replace_once(
    """        ByteSamplingState &state = thread.bytes;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        const auto current_tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
""",
    """        ByteSamplingState &state = thread.bytes;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        const std::uint64_t current_tid = thread.owner_tid.load(std::memory_order_relaxed);
""",
)

# Sampled-pointer presence filter: almost every free is for an unsampled
# allocation. Avoid touching a lifecycle shard lock when the exact hash bucket
# contains no sampled pointer. Sampled pointers still take the original locked
# path, preserving cross-thread free/realloc lifecycle semantics.
replace_once(
    "constexpr std::size_t KLiveIndexShardCapacity = KLiveIndexCapacity / KLiveIndexShards;\n"
    "constexpr std::size_t KMaxSampledThreads = 256;",
    "constexpr std::size_t KLiveIndexShardCapacity = KLiveIndexCapacity / KLiveIndexShards;\n"
    "constexpr std::size_t KLivePresenceBuckets = KLiveIndexCapacity;\n"
    "constexpr std::size_t KMaxSampledThreads = 256;",
)

replace_once(
    "    std::array<pthread_rwlock_t, KLiveIndexShards> live_index_locks{};\n"
    "    pthread_mutex_t live_pool_mutex = PTHREAD_MUTEX_INITIALIZER;",
    "    std::array<pthread_rwlock_t, KLiveIndexShards> live_index_locks{};\n"
    "    std::array<std::atomic<std::uint32_t>, KLivePresenceBuckets> live_presence{};\n"
    "    pthread_mutex_t live_pool_mutex = PTHREAD_MUTEX_INITIALIZER;",
)

replace_once(
    """    static std::size_t liveIndexShard(std::uint64_t hash) noexcept
    {
        return static_cast<std::size_t>(hash & (KLiveIndexShards - 1));
    }

    static std::size_t liveIndexSlot(std::uint64_t hash, std::size_t shard, std::size_t offset = 0) noexcept""",
    """    static std::size_t liveIndexShard(std::uint64_t hash) noexcept
    {
        return static_cast<std::size_t>(hash & (KLiveIndexShards - 1));
    }

    static std::size_t livePresenceSlot(std::uint64_t hash) noexcept
    {
        return static_cast<std::size_t>((hash >> 6) & (KLivePresenceBuckets - 1));
    }

    static std::size_t liveIndexSlot(std::uint64_t hash, std::size_t shard, std::size_t offset = 0) noexcept""",
)

replace_once(
    """    LiveAllocation *detachAllocation(void *pointer) noexcept
    {
        if (pointer == nullptr || live_index == nullptr) {
            return nullptr;
        }
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t shard = liveIndexShard(hash);
        if (config.force_live_lock_contention_for_testing ||
            ::pthread_rwlock_trywrlock(&live_index_locks[shard]) != 0) {""",
    """    LiveAllocation *detachAllocation(void *pointer) noexcept
    {
        if (pointer == nullptr || live_index == nullptr) {
            return nullptr;
        }
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t presence_slot = livePresenceSlot(hash);
        if (!config.force_live_lock_contention_for_testing &&
            live_presence[presence_slot].load(std::memory_order_acquire) == 0) {
            return nullptr;
        }
        const std::size_t shard = liveIndexShard(hash);
        if (config.force_live_lock_contention_for_testing ||
            ::pthread_rwlock_trywrlock(&live_index_locks[shard]) != 0) {""",
)

replace_once(
    """                detached = entry_allocation;
                clearEntry(entry);
                break;""",
    """                detached = entry_allocation;
                clearEntry(entry);
                live_presence[presence_slot].fetch_sub(1, std::memory_order_release);
                break;""",
)

replace_once(
    """        const std::uint64_t weight = allocation->weight_bytes;
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t shard = liveIndexShard(hash);""",
    """        const std::uint64_t weight = allocation->weight_bytes;
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t presence_slot = livePresenceSlot(hash);
        const std::size_t shard = liveIndexShard(hash);""",
)

replace_once(
    """        LiveAllocation *replaced = nullptr;
        bool inserted = false;
        std::size_t tombstone = KLiveIndexCapacity;""",
    """        LiveAllocation *replaced = nullptr;
        bool inserted = false;
        bool added_presence = false;
        std::size_t tombstone = KLiveIndexCapacity;""",
)

replace_once(
    """                publishEntry(destination, pointer, allocation_id, allocation);
                inserted = true;
                break;""",
    """                publishEntry(destination, pointer, allocation_id, allocation);
                inserted = true;
                added_presence = true;
                break;""",
)

replace_once(
    """            publishEntry(live_index[tombstone], pointer, allocation_id, allocation);
            inserted = true;
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);""",
    """            publishEntry(live_index[tombstone], pointer, allocation_id, allocation);
            inserted = true;
            added_presence = true;
        }
        if (added_presence) {
            live_presence[presence_slot].fetch_add(1, std::memory_order_release);
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);""",
)

replace_once(
    "        lifecycle_writers.store(0, std::memory_order_relaxed);\n"
    "        deferred_live.store(nullptr, std::memory_order_relaxed);",
    "        lifecycle_writers.store(0, std::memory_order_relaxed);\n"
    "        for (auto &presence : live_presence) {\n"
    "            presence.store(0, std::memory_order_relaxed);\n"
    "        }\n"
    "        deferred_live.store(nullptr, std::memory_order_relaxed);",
)

path.write_text(text)
