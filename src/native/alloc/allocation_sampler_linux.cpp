#include "native/alloc/allocation_sampler.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "allocation_sampler_linux.cpp requires Linux x86-64"
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cpptrace/cpptrace.hpp>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "native/alloc/allocation_profile_aggregation.h"
#include "native/alloc/bounded_event_queue.h"
#include "native/alloc/byte_sampler.h"
#include "native/alloc/elf_import_hooks.h"
#include "native/alloc/stable_shard_snapshot.h"
#include "native/sampler/thread_info.h"
#include "profiling_window.h"

namespace spark {
namespace {

constexpr std::size_t KStackDepth = 48;
constexpr std::size_t KEventCapacity = 16384;
constexpr std::size_t KLiveIndexCapacity = KEventCapacity * 2;
constexpr std::size_t KLiveIndexShards = 64;
constexpr std::size_t KLiveIndexShardCapacity = KLiveIndexCapacity / KLiveIndexShards;
constexpr std::size_t KLivePresenceBuckets = KLiveIndexCapacity;
constexpr std::size_t KHookCallShards = 1024;
constexpr std::size_t KLiveLockAttempts = 64;
constexpr std::size_t KMaxSampledThreads = 256;
constexpr std::size_t KMaxAllocationModules = 512;
constexpr std::size_t KMaxProfileNodes = 131072;
constexpr std::size_t KMaxPendingSamples = 32768;
constexpr std::size_t KMaxTickDecisions = 100000;
constexpr std::size_t KTickEventCapacity = 4096;
constexpr std::size_t KFramesToSkip = 4;

void *tombstonePointer() noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(1));
}

std::uint64_t monotonicMs() noexcept
{
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000 + static_cast<std::uint64_t>(value.tv_nsec) / 1000000;
}

std::uint64_t saturatingMultiply(std::uint64_t a, std::uint64_t b) noexcept
{
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > maximum / b ? maximum : a * b;
}

bool checkedMultiply(std::size_t a, std::size_t b, std::uint64_t &out) noexcept
{
    if (a != 0 && b > (std::numeric_limits<std::size_t>::max)() / a) {
        out = 0;
        return false;
    }
    out = static_cast<std::uint64_t>(a * b);
    return true;
}

}  // namespace

struct AllocationSampler::Impl {
    using MallocFn = void *(*)(std::size_t);
    using CallocFn = void *(*)(std::size_t, std::size_t);
    using ReallocFn = void *(*)(void *, std::size_t);
    using FreeFn = void (*)(void *);
    using ReallocArrayFn = void *(*)(void *, std::size_t, std::size_t);
    using AlignedAllocFn = void *(*)(std::size_t, std::size_t);
    using PosixMemalignFn = int (*)(void **, std::size_t, std::size_t);

    struct AllocationEvent {
        std::uint64_t weight_bytes = 0;
        std::uint64_t tick_id = 0;
        std::uint64_t thread_id = 0;
        std::uint64_t os_thread_id = 0;
        std::int32_t window = 0;
        std::uint16_t depth = 0;
        bool thread_observation = false;
        cpptrace::frame_ptr frames[KStackDepth]{};
    };

    struct TickEvent {
        std::uint64_t tick_id = 0;
        double mspt_ms = 0.0;
    };

    struct LiveAllocation {
        LiveAllocation *next = nullptr;
        void *pointer = nullptr;
        std::uint64_t allocation_id = 0;
        std::uint64_t weight_bytes = 0;
        std::uint64_t requested_bytes = 0;
        std::uint64_t allocated_ms = 0;
        std::uint64_t tick_id = 0;
        std::uint64_t thread_id = 0;
        std::uint64_t os_thread_id = 0;
        std::int32_t window = 0;
        std::uint16_t depth = 0;
        cpptrace::frame_ptr frames[KStackDepth]{};
    };

    struct LiveIndexEntry {
        void *pointer = nullptr;
        std::uint64_t allocation_id = 0;
        LiveAllocation *allocation = nullptr;
    };

    struct EventQueue {
        struct Cell {
            std::atomic<std::size_t> sequence{0};
            AllocationEvent event;
        };

        Cell *storage = nullptr;
        std::atomic<std::size_t> producer{0};
        std::atomic<std::size_t> consumer{0};
        std::atomic<std::uint64_t> size{0};
        std::atomic<std::uint64_t> high_water{0};

        bool allocate(std::string &error)
        {
            const std::size_t bytes = sizeof(Cell) * KEventCapacity;
            void *memory = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (memory == MAP_FAILED) {
                storage = nullptr;
                error = "mmap for Linux allocation sample buffer failed: " + std::string(std::strerror(errno));
                return false;
            }
            storage = static_cast<Cell *>(memory);
            for (std::size_t i = 0; i < KEventCapacity; ++i) {
                ::new (static_cast<void *>(&storage[i])) Cell{};
                storage[i].sequence.store(i, std::memory_order_relaxed);
            }
            producer.store(0, std::memory_order_relaxed);
            consumer.store(0, std::memory_order_relaxed);
            size.store(0, std::memory_order_relaxed);
            high_water.store(0, std::memory_order_relaxed);
            return true;
        }

        void release() noexcept
        {
            if (storage != nullptr) {
                ::munmap(storage, sizeof(Cell) * KEventCapacity);
                storage = nullptr;
            }
            producer.store(0, std::memory_order_relaxed);
            consumer.store(0, std::memory_order_relaxed);
            size.store(0, std::memory_order_relaxed);
        }

        bool enqueue(const AllocationEvent &event) noexcept
        {
            std::size_t position = producer.load(std::memory_order_relaxed);
            Cell *cell = nullptr;
            bool reserved = false;
            for (std::size_t attempt = 0; attempt < KBoundedEventQueueMaxAttempts; ++attempt) {
                cell = &storage[position & (KEventCapacity - 1)];
                const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
                const std::intptr_t difference =
                    static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
                if (difference == 0) {
                    if (producer.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                        reserved = true;
                        break;
                    }
                }
                else if (difference < 0) {
                    return false;
                }
                else {
                    position = producer.load(std::memory_order_relaxed);
                }
            }
            if (!reserved) {
                return false;
            }
            const std::uint64_t current = size.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t previous = high_water.load(std::memory_order_relaxed);
            for (std::size_t attempt = 0; attempt < KBoundedEventQueueMaxAttempts && previous < current; ++attempt) {
                if (high_water.compare_exchange_weak(previous, current, std::memory_order_relaxed)) {
                    break;
                }
            }
            cell->event = event;
            cell->sequence.store(position + 1, std::memory_order_release);
            return true;
        }

        bool dequeue(AllocationEvent &event) noexcept
        {
            std::size_t position = consumer.load(std::memory_order_relaxed);
            Cell *cell = nullptr;
            bool reserved = false;
            for (std::size_t attempt = 0; attempt < KBoundedEventQueueMaxAttempts; ++attempt) {
                cell = &storage[position & (KEventCapacity - 1)];
                const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
                const std::intptr_t difference =
                    static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position + 1);
                if (difference == 0) {
                    if (consumer.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                        reserved = true;
                        break;
                    }
                }
                else if (difference < 0) {
                    return false;
                }
                else {
                    position = consumer.load(std::memory_order_relaxed);
                }
            }
            if (!reserved) {
                return false;
            }
            event = cell->event;
            size.fetch_sub(1, std::memory_order_relaxed);
            cell->sequence.store(position + KEventCapacity, std::memory_order_release);
            return true;
        }
    };

    struct ThreadSamplingState {
        std::atomic<std::uint8_t> registry_state{0};
        std::atomic<std::uint64_t> owner_tid{0};
        ByteSamplingState bytes;
        std::uint64_t identity_generation = 0;
        std::uint64_t session_thread_id = 0;
        std::uint64_t os_thread_id = 0;
        bool inside_hook = false;
        bool tracking_suppressed = false;
        bool identity_announced = false;
    };

    inline static thread_local bool mCountOnlyInsideHook = false;

    static std::size_t currentHookShard() noexcept
    {
        std::uintptr_t value = reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer());
        value ^= value >> 17;
        value *= 0x9e3779b97f4a7c15ULL;
        value ^= value >> 29;
        return static_cast<std::size_t>(value & (KHookCallShards - 1));
    }

    class HookCallGuard {
    public:
        HookCallGuard() noexcept : shard_(currentHookShard())
        {
            mActiveHookCalls[shard_].fetch_add(1, std::memory_order_acq_rel);
        }
        ~HookCallGuard() { mActiveHookCalls[shard_].fetch_sub(1, std::memory_order_release); }

    private:
        std::size_t shard_ = 0;
    };

    class TrackingCallGuard {
    public:
        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl), shard_(currentHookShard())
        {
            if (!impl_.tracking.load(std::memory_order_acquire)) {
                return;
            }
            impl_.tracking_calls[shard_].fetch_add(1, std::memory_order_acq_rel);
            if (impl_.tracking.load(std::memory_order_acquire)) {
                active_ = true;
            }
            else {
                impl_.tracking_calls[shard_].fetch_sub(1, std::memory_order_release);
            }
        }
        ~TrackingCallGuard()
        {
            if (active_) {
                impl_.tracking_calls[shard_].fetch_sub(1, std::memory_order_release);
            }
        }
        explicit operator bool() const noexcept { return active_; }

    private:
        Impl &impl_;
        std::size_t shard_ = 0;
        bool active_ = false;
    };

    class RecursionGuard {
    public:
        explicit RecursionGuard(Impl &impl) noexcept
        {
            if (impl.config.count_only) {
                if (!mCountOnlyInsideHook) {
                    mCountOnlyInsideHook = true;
                    count_only_owner_ = true;
                    owner_ = true;
                }
                return;
            }
            state_ = impl.currentThreadState();
            if (state_ != nullptr && !state_->inside_hook) {
                state_->inside_hook = true;
                owner_ = true;
            }
        }

        ~RecursionGuard()
        {
            if (count_only_owner_) {
                mCountOnlyInsideHook = false;
                return;
            }
            if (owner_) {
                state_->inside_hook = false;
            }
        }

        [[nodiscard]] bool owner() const noexcept { return owner_; }

    private:
        ThreadSamplingState *state_ = nullptr;
        bool count_only_owner_ = false;
        bool owner_ = false;
    };

    class TrackingSuppressionGuard {
    public:
        explicit TrackingSuppressionGuard(Impl &impl) noexcept : state_(impl.currentThreadState())
        {
            if (state_ != nullptr) {
                previous_ = state_->tracking_suppressed;
                state_->tracking_suppressed = true;
            }
        }
        ~TrackingSuppressionGuard()
        {
            if (state_ != nullptr) {
                state_->tracking_suppressed = previous_;
            }
        }

    private:
        ThreadSamplingState *state_ = nullptr;
        bool previous_ = false;
    };

    static std::atomic<Impl *> mActiveInstance;
    static std::array<std::atomic<std::uint64_t>, KHookCallShards> mActiveHookCalls;

    ElfImportHooks hooks;
    MallocFn real_malloc = nullptr;
    CallocFn real_calloc = nullptr;
    ReallocFn real_realloc = nullptr;
    FreeFn real_free = nullptr;
    ReallocArrayFn real_reallocarray = nullptr;
    AlignedAllocFn real_aligned_alloc = nullptr;
    PosixMemalignFn real_posix_memalign = nullptr;
    std::vector<AllocationHookCapability> hook_capabilities;

    std::mutex lifecycle_mutex;
    std::timed_mutex aggregate_mutex;
    AllocationSamplerConfig config{};
    std::atomic<bool> tracking{false};
    std::atomic<bool> running{false};
    std::atomic<bool> aggregator_running{false};
    std::atomic<bool> aggregator_failed{false};
    std::array<char, 256> aggregator_failure{};
    std::array<std::atomic<std::uint64_t>, KHookCallShards> tracking_calls{};
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> interval_bytes{kDefaultAllocationIntervalBytes};
    std::atomic<std::uint64_t> sampling_seed{0};
    struct alignas(64) HotCounters {
        std::atomic<std::uint64_t> hook_calls{0};
        std::atomic<std::uint64_t> successful_allocation_calls{0};
        std::atomic<std::uint64_t> observed_bytes{0};
    };
    static_assert(sizeof(HotCounters) <= 64);
    std::array<HotCounters, KHookCallShards> hot_counters{};
    std::atomic<std::uint64_t> sampling_points{0};
    std::atomic<std::uint64_t> filtered_samples{0};
    std::atomic<std::uint64_t> dropped_samples{0};
    std::atomic<std::uint64_t> dropped_events{0};
    std::atomic<std::uint64_t> dropped_tick_events{0};
    std::atomic<std::uint64_t> enqueued_samples{0};
    std::atomic<std::uint64_t> next_allocation_id{1};
    std::atomic<std::uint64_t> next_session_thread_id{1};
    std::atomic<std::uint64_t> registered_threads{0};
    std::atomic<std::uint64_t> overflow_threads{0};
    std::atomic<std::uint64_t> thread_state_drops{0};
    std::atomic<std::uint64_t> freed_samples{0};
    std::atomic<std::uint64_t> freed_bytes{0};
    std::atomic<std::uint64_t> live_samples{0};
    std::atomic<std::uint64_t> live_bytes{0};
    std::atomic<std::uint64_t> peak_live_samples{0};
    std::atomic<std::uint64_t> lifetime_ms_total{0};
    std::atomic<std::uint64_t> lifetime_ms_max{0};
    std::atomic<std::uint64_t> lifecycle_dropped{0};
    std::atomic<std::uint64_t> contention_dropped{0};
    std::atomic<std::uint64_t> lifecycle_version{0};
    std::atomic<std::uint64_t> lifecycle_readers{0};
    std::atomic<std::uint64_t> lifecycle_writers{0};
    std::atomic<std::uint64_t> retained_age_ms_total{0};
    std::atomic<std::uint64_t> retained_age_ms_max{0};
    std::uint64_t last_module_rescan_ms = 0;

    std::array<pthread_rwlock_t, KLiveIndexShards> live_index_locks{};
    std::array<std::atomic<std::uint32_t>, KLivePresenceBuckets> live_presence{};
    pthread_mutex_t live_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_key_t thread_state_key{};
    bool thread_state_key_created = false;
    std::array<ThreadSamplingState, 2048> thread_states{};
    LiveAllocation *live_storage = nullptr;
    LiveAllocation *free_live = nullptr;
    std::atomic<LiveAllocation *> deferred_live{nullptr};
    LiveIndexEntry *live_index = nullptr;

    Impl()
    {
        for (pthread_rwlock_t &lock : live_index_locks) {
            ::pthread_rwlock_init(&lock, nullptr);
        }
    }

    ~Impl()
    {
        for (pthread_rwlock_t &lock : live_index_locks) {
            ::pthread_rwlock_destroy(&lock);
        }
        ::pthread_mutex_destroy(&live_pool_mutex);
    }

    EventQueue events;
    std::thread aggregator_thread;
    BoundedEventQueue<TickEvent, KTickEventCapacity> ticks;
    AllocationProfileAggregation aggregation;

    RecoverySink *recovery_sink = nullptr;

    static Impl *activeOrAbort() noexcept
    {
        Impl *impl = mActiveInstance.load(std::memory_order_acquire);
        if (impl == nullptr) {
            std::abort();
        }
        if (impl->tracking.load(std::memory_order_relaxed)) {
            impl->hot_counters[currentHookShard()].hook_calls.fetch_add(1, std::memory_order_relaxed);
        }
        return impl;
    }

    static void *hookMalloc(std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleMalloc(size);
    }

    static void *hookCalloc(std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleCalloc(count, size);
    }

    static void *hookRealloc(void *pointer, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleRealloc(pointer, size);
    }

    static void hookFree(void *pointer) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        impl->handleFree(pointer);
    }

    static void *hookReallocArray(void *pointer, std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleReallocArray(pointer, count, size);
    }

    static void *hookAlignedAlloc(std::size_t alignment, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handleAlignedAlloc(alignment, size);
    }

    static int hookPosixMemalign(void **result, std::size_t alignment, std::size_t size) noexcept
    {
        HookCallGuard guard;
        Impl *impl = activeOrAbort();
        return impl->handlePosixMemalign(result, alignment, size);
    }

    static void releaseThreadState(void *value) noexcept
    {
        if (value == nullptr || value == tombstonePointer()) {
            return;
        }
        auto *state = static_cast<ThreadSamplingState *>(value);
        state->inside_hook = false;
        state->tracking_suppressed = false;
        state->owner_tid.store(0, std::memory_order_relaxed);
        state->registry_state.store(0, std::memory_order_release);
    }

    ThreadSamplingState *currentThreadState() noexcept
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

    bool shouldTrackCurrentThread() const noexcept
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
    {
        const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer) >> 4);
        return value * 11400714819323198485ULL;
    }

    static std::size_t liveIndexShard(std::uint64_t hash) noexcept
    {
        return static_cast<std::size_t>(hash & (KLiveIndexShards - 1));
    }

    static std::size_t livePresenceSlot(std::uint64_t hash) noexcept
    {
        return static_cast<std::size_t>((hash >> 6) & (KLivePresenceBuckets - 1));
    }

    static std::size_t liveIndexSlot(std::uint64_t hash, std::size_t shard, std::size_t offset = 0) noexcept
    {
        const std::size_t within = (static_cast<std::size_t>(hash >> 6) + offset) & (KLiveIndexShardCapacity - 1);
        return shard * KLiveIndexShardCapacity + within;
    }

    static void *entryPointer(const LiveIndexEntry &entry) noexcept
    {
        return std::atomic_ref<void *>(const_cast<void *&>(entry.pointer)).load(std::memory_order_acquire);
    }

    static LiveAllocation *entryAllocation(const LiveIndexEntry &entry) noexcept
    {
        return std::atomic_ref<LiveAllocation *>(const_cast<LiveAllocation *&>(entry.allocation))
            .load(std::memory_order_acquire);
    }

    static std::uint64_t entryAllocationId(const LiveIndexEntry &entry) noexcept
    {
        return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t &>(entry.allocation_id))
            .load(std::memory_order_relaxed);
    }

    static void publishEntry(LiveIndexEntry &entry, void *pointer, std::uint64_t allocation_id,
                             LiveAllocation *allocation) noexcept
    {
        std::atomic_ref<std::uint64_t>(entry.allocation_id).store(allocation_id, std::memory_order_relaxed);
        std::atomic_ref<LiveAllocation *>(entry.allocation).store(allocation, std::memory_order_relaxed);
        std::atomic_ref<void *>(entry.pointer).store(pointer, std::memory_order_release);
    }

    static void clearEntry(LiveIndexEntry &entry) noexcept
    {
        std::atomic_ref<void *>(entry.pointer).store(tombstonePointer(), std::memory_order_release);
        std::atomic_ref<std::uint64_t>(entry.allocation_id).store(0, std::memory_order_relaxed);
        std::atomic_ref<LiveAllocation *>(entry.allocation).store(nullptr, std::memory_order_relaxed);
    }

    void retireAllocation(LiveAllocation *allocation, std::uint64_t released_ms) noexcept
    {
        freed_samples.fetch_add(1, std::memory_order_relaxed);
        freed_bytes.fetch_add(allocation->weight_bytes, std::memory_order_relaxed);
        live_samples.fetch_sub(1, std::memory_order_relaxed);
        live_bytes.fetch_sub(allocation->weight_bytes, std::memory_order_relaxed);
        const std::uint64_t lifetime =
            released_ms >= allocation->allocated_ms ? released_ms - allocation->allocated_ms : 0;
        lifetime_ms_total.fetch_add(lifetime, std::memory_order_relaxed);
        std::uint64_t previous = lifetime_ms_max.load(std::memory_order_relaxed);
        while (previous < lifetime &&
               !lifetime_ms_max.compare_exchange_weak(previous, lifetime, std::memory_order_relaxed)) {
        }
        recycleLiveRecord(allocation);
    }

    LiveAllocation *detachAllocation(void *pointer) noexcept
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
        if (!tryLockLiveIndexShard(shard)) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        lifecycle_writers.fetch_add(1, std::memory_order_acq_rel);
        lifecycle_version.fetch_add(1, std::memory_order_release);
        LiveAllocation *detached = nullptr;
        for (std::size_t offset = 0; offset < KLiveIndexShardCapacity; ++offset) {
            LiveIndexEntry &entry = live_index[liveIndexSlot(hash, shard, offset)];
            void *entry_pointer = entryPointer(entry);
            if (entry_pointer == nullptr) {
                break;
            }
            if (entry_pointer == pointer) {
                LiveAllocation *entry_allocation = entryAllocation(entry);
                if (entry_allocation == nullptr || entryAllocationId(entry) != entry_allocation->allocation_id) {
                    lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                detached = entry_allocation;
                clearEntry(entry);
                live_presence[presence_slot].fetch_sub(1, std::memory_order_release);
                break;
            }
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);
        lifecycle_writers.fetch_sub(1, std::memory_order_release);
        ::pthread_rwlock_unlock(&live_index_locks[shard]);
        return detached;
    }

    void restoreDetachedAllocation(LiveAllocation *allocation) noexcept
    {
        if (allocation == nullptr) {
            return;
        }
        const std::uint64_t weight = allocation->weight_bytes;
        if (!insertLiveAllocation(allocation, false)) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            live_samples.fetch_sub(1, std::memory_order_relaxed);
            live_bytes.fetch_sub(weight, std::memory_order_relaxed);
            recycleLiveRecord(allocation);
        }
    }

    void handleFree(void *pointer) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            real_free(pointer);
            return;
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            real_free(pointer);
            return;
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        real_free(pointer);
        if (allocation != nullptr) {
            retireAllocation(allocation, monotonicMs());
        }
    }

    void *handleMalloc(std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_malloc(size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return real_malloc(size);
        }
        void *result = real_malloc(size);
        if (result != nullptr) {
            recordAllocation(result, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    void *handleCalloc(std::size_t count, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_calloc(count, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return real_calloc(count, size);
        }
        void *result = real_calloc(count, size);
        std::uint64_t bytes = 0;
        if (result != nullptr && checkedMultiply(count, size, bytes)) {
            recordAllocation(result, bytes);
        }
        return result;
    }

    void *handleRealloc(void *pointer, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_realloc(pointer, size);
        }
        TrackingCallGuard tracking_guard(*this);
        if (!tracking_guard) {
            return real_realloc(pointer, size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return real_realloc(pointer, size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *result = real_realloc(pointer, size);
        const bool replaced = result != nullptr || (pointer != nullptr && size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (result != nullptr && size != 0) {
            recordAllocation(result, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    void *handleReallocArray(void *pointer, std::size_t count, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_reallocarray(pointer, count, size);
        }
        TrackingCallGuard tracking_guard(*this);
        if (!tracking_guard) {
            return real_reallocarray(pointer, count, size);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return real_reallocarray(pointer, count, size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *result = real_reallocarray(pointer, count, size);
        const bool replaced = result != nullptr || (pointer != nullptr && valid_size && bytes == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (result != nullptr && valid_size && bytes != 0) {
            recordAllocation(result, bytes);
        }
        return result;
    }

    void *handleAlignedAlloc(std::size_t alignment, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_aligned_alloc(alignment, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return real_aligned_alloc(alignment, size);
        }
        void *result = real_aligned_alloc(alignment, size);
        if (result != nullptr) {
            recordAllocation(result, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    int handlePosixMemalign(void **result_pointer, std::size_t alignment, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_posix_memalign(result_pointer, alignment, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion(*this);
        if (!tracking_guard || !recursion.owner()) {
            return real_posix_memalign(result_pointer, alignment, size);
        }
        const int result = real_posix_memalign(result_pointer, alignment, size);
        if (result == 0 && result_pointer != nullptr && *result_pointer != nullptr) {
            recordAllocation(*result_pointer, static_cast<std::uint64_t>(size));
        }
        return result;
    }

    LiveAllocation *acquireLiveRecord() noexcept
    {
        if (!tryLockLivePool()) {
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (free_live == nullptr && lifecycle_readers.load(std::memory_order_acquire) == 0) {
            free_live = deferred_live.exchange(nullptr, std::memory_order_acq_rel);
        }
        LiveAllocation *record = free_live;
        if (record != nullptr) {
            free_live = record->next;
            record->next = nullptr;
        }
        ::pthread_mutex_unlock(&live_pool_mutex);
        return record;
    }

    void accountLiveAllocation(std::uint64_t weight) noexcept
    {
        const std::uint64_t current_live = live_samples.fetch_add(1, std::memory_order_relaxed) + 1;
        live_bytes.fetch_add(weight, std::memory_order_relaxed);
        std::uint64_t previous_peak = peak_live_samples.load(std::memory_order_relaxed);
        while (previous_peak < current_live &&
               !peak_live_samples.compare_exchange_weak(previous_peak, current_live, std::memory_order_relaxed)) {
        }
    }

    bool insertLiveAllocation(LiveAllocation *allocation, bool account_live) noexcept
    {
        void *pointer = allocation->pointer;
        const std::uint64_t allocation_id = allocation->allocation_id;
        const std::uint64_t weight = allocation->weight_bytes;
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t presence_slot = livePresenceSlot(hash);
        const std::size_t shard = liveIndexShard(hash);
        if (!tryLockLiveIndexShard(shard)) {
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        lifecycle_writers.fetch_add(1, std::memory_order_acq_rel);
        lifecycle_version.fetch_add(1, std::memory_order_release);
        LiveAllocation *replaced = nullptr;
        bool inserted = false;
        bool added_presence = false;
        std::size_t tombstone = KLiveIndexCapacity;
        for (std::size_t offset = 0; offset < KLiveIndexShardCapacity; ++offset) {
            const std::size_t slot = liveIndexSlot(hash, shard, offset);
            LiveIndexEntry &entry = live_index[slot];
            void *entry_pointer = entryPointer(entry);
            if (entry_pointer == tombstonePointer()) {
                if (tombstone == KLiveIndexCapacity) {
                    tombstone = slot;
                }
                continue;
            }
            if (entry_pointer == pointer) {
                replaced = entryAllocation(entry);
                std::atomic_ref<void *>(entry.pointer).store(tombstonePointer(), std::memory_order_release);
                if (account_live) {
                    accountLiveAllocation(weight);
                }
                publishEntry(entry, pointer, allocation_id, allocation);
                inserted = true;
                break;
            }
            if (entry_pointer == nullptr) {
                LiveIndexEntry &destination = live_index[tombstone != KLiveIndexCapacity ? tombstone : slot];
                if (account_live) {
                    accountLiveAllocation(weight);
                }
                publishEntry(destination, pointer, allocation_id, allocation);
                inserted = true;
                added_presence = true;
                break;
            }
        }
        if (!inserted && tombstone != KLiveIndexCapacity) {
            if (account_live) {
                accountLiveAllocation(weight);
            }
            publishEntry(live_index[tombstone], pointer, allocation_id, allocation);
            inserted = true;
            added_presence = true;
        }
        if (added_presence) {
            live_presence[presence_slot].fetch_add(1, std::memory_order_release);
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);
        lifecycle_writers.fetch_sub(1, std::memory_order_release);
        ::pthread_rwlock_unlock(&live_index_locks[shard]);

        if (replaced != nullptr) {
            // Pointer reuse retires lifecycle records missed by patched imports.
            retireAllocation(replaced, monotonicMs());
        }
        return inserted;
    }

    void recycleLiveRecord(LiveAllocation *allocation) noexcept
    {
        if (lifecycle_readers.load(std::memory_order_acquire) != 0) {
            LiveAllocation *head = deferred_live.load(std::memory_order_relaxed);
            do {
                allocation->next = head;
            } while (!deferred_live.compare_exchange_weak(head, allocation, std::memory_order_release,
                                                          std::memory_order_relaxed));
            return;
        }
        if (!tryLockLivePool()) {
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        allocation->next = free_live;
        free_live = allocation;
        ::pthread_mutex_unlock(&live_pool_mutex);
    }

    void recordAllocation(void *pointer, std::uint64_t requested_bytes) noexcept
    {
        HotCounters &counters = hot_counters[currentHookShard()];
        counters.successful_allocation_calls.fetch_add(1, std::memory_order_relaxed);
        if (requested_bytes == 0) {
            return;
        }
        counters.observed_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
        if (config.count_only) {
            return;
        }
        ThreadSamplingState *thread_pointer = currentThreadState();
        if (thread_pointer == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ThreadSamplingState &thread = *thread_pointer;
        ByteSamplingState &state = thread.bytes;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        const std::uint64_t current_tid = thread.owner_tid.load(std::memory_order_relaxed);
        if (state.generation != current_generation) {
            resetByteSamplingState(state, current_generation,
                                   sampling_seed.load(std::memory_order_relaxed) ^ current_generation ^ current_tid,
                                   interval);
        }
        if (thread.identity_generation != current_generation) {
            thread.identity_generation = current_generation;
            thread.os_thread_id = current_tid;
            const std::uint64_t next = next_session_thread_id.fetch_add(1, std::memory_order_relaxed);
            thread.session_thread_id = next;
            thread.identity_announced = false;
            if (next <= KMaxSampledThreads) {
                registered_threads.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                overflow_threads.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const std::uint64_t points = consumeSampledBytes(state, requested_bytes, interval);
        if (points == 0) {
            return;
        }
        sampling_points.fetch_add(points, std::memory_order_relaxed);

        LiveAllocation *allocation = acquireLiveRecord();
        if (allocation == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        allocation->pointer = pointer;
        allocation->allocation_id = next_allocation_id.fetch_add(1, std::memory_order_relaxed);
        allocation->weight_bytes = saturatingMultiply(points, interval);
        allocation->requested_bytes = requested_bytes;
        allocation->allocated_ms = monotonicMs();
        allocation->tick_id = current_tick.load(std::memory_order_relaxed);
        allocation->thread_id = thread.session_thread_id;
        allocation->os_thread_id = thread.os_thread_id;
        allocation->window = profiling_window::windowNow();
        allocation->depth = static_cast<std::uint16_t>(
            cpptrace::safe_generate_raw_trace(allocation->frames, KStackDepth, KFramesToSkip));
        const std::uint64_t allocation_weight = allocation->weight_bytes;
        const std::uint64_t allocation_tick = allocation->tick_id;
        const std::uint64_t allocation_thread = allocation->thread_id;
        const std::uint64_t allocation_os_thread = allocation->os_thread_id;
        const std::int32_t allocation_window = allocation->window;
        const std::uint16_t allocation_depth = allocation->depth;
        const bool live_only = config.live_only;
        AllocationEvent event{};
        event.thread_id = allocation_thread;
        event.os_thread_id = allocation_os_thread;
        event.thread_observation = live_only;
        if (!live_only) {
            event.weight_bytes = allocation_weight;
            event.tick_id = allocation_tick;
            event.window = allocation_window;
            event.depth = allocation_depth;
            std::memcpy(event.frames, allocation->frames,
                        static_cast<std::size_t>(allocation_depth) * sizeof(cpptrace::frame_ptr));
        }
        const bool inserted = allocation_depth != 0 && insertLiveAllocation(allocation, true);
        if (!inserted) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            recycleLiveRecord(allocation);
            return;
        }

        if (live_only) {
            if (!thread.identity_announced) {
                if (events.enqueue(event)) {
                    thread.identity_announced = true;
                }
                else {
                    dropped_samples.fetch_add(1, std::memory_order_relaxed);
                    dropped_events.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        if (!events.enqueue(event)) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            dropped_events.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        enqueued_samples.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename Function>
    bool resolveAllocator(const char *name, Function &function, bool required, std::string &error)
    {
        ::dlerror();
        // Preserve LD_PRELOAD interposition when resolving the effective allocator.
        function = reinterpret_cast<Function>(::dlsym(RTLD_DEFAULT, name));
        const char *failure = ::dlerror();
        if (function == nullptr && required) {
            error = std::string("required Linux allocator symbol not found: ") + name;
            if (failure != nullptr) {
                error += ": ";
                error += failure;
            }
            return false;
        }
        return true;
    }

    bool allocateLifecycleStorage(std::string &error)
    {
        void *records = ::mmap(nullptr, sizeof(LiveAllocation) * KEventCapacity, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *index = ::mmap(nullptr, sizeof(LiveIndexEntry) * KLiveIndexCapacity, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (records == MAP_FAILED || index == MAP_FAILED) {
            if (records != MAP_FAILED) {
                ::munmap(records, sizeof(LiveAllocation) * KEventCapacity);
            }
            if (index != MAP_FAILED) {
                ::munmap(index, sizeof(LiveIndexEntry) * KLiveIndexCapacity);
            }
            error = "mmap for Linux allocation lifecycle tracking failed: " + std::string(std::strerror(errno));
            return false;
        }
        live_storage = static_cast<LiveAllocation *>(records);
        live_index = static_cast<LiveIndexEntry *>(index);
        free_live = nullptr;
        deferred_live.store(nullptr, std::memory_order_relaxed);
        for (std::size_t i = 0; i < KEventCapacity; ++i) {
            live_storage[i].next = free_live;
            free_live = &live_storage[i];
        }
        return true;
    }

    void releaseLifecycleStorage() noexcept
    {
        if (live_storage != nullptr) {
            ::munmap(live_storage, sizeof(LiveAllocation) * KEventCapacity);
            live_storage = nullptr;
        }
        if (live_index != nullptr) {
            ::munmap(live_index, sizeof(LiveIndexEntry) * KLiveIndexCapacity);
            live_index = nullptr;
        }
        free_live = nullptr;
        deferred_live.store(nullptr, std::memory_order_relaxed);
    }

    bool prepareHooks(std::string &error)
    {
        if (!thread_state_key_created) {
            if (::pthread_key_create(&thread_state_key, &releaseThreadState) != 0) {
                error = "pthread_key_create for allocation thread state failed";
                return false;
            }
            thread_state_key_created = true;
        }
        if (real_malloc == nullptr) {
            if (!resolveAllocator("malloc", real_malloc, true, error) ||
                !resolveAllocator("calloc", real_calloc, true, error) ||
                !resolveAllocator("realloc", real_realloc, true, error) ||
                !resolveAllocator("free", real_free, true, error) ||
                !resolveAllocator("reallocarray", real_reallocarray, true, error) ||
                !resolveAllocator("aligned_alloc", real_aligned_alloc, true, error) ||
                !resolveAllocator("posix_memalign", real_posix_memalign, true, error)) {
                return false;
            }
        }

        const std::array specs{
            ElfImportHookSpec{.name = "malloc", .replacement = reinterpret_cast<void *>(&hookMalloc), .required = true},
            ElfImportHookSpec{.name = "calloc", .replacement = reinterpret_cast<void *>(&hookCalloc), .required = true},
            ElfImportHookSpec{
                .name = "realloc", .replacement = reinterpret_cast<void *>(&hookRealloc), .required = true},
            ElfImportHookSpec{.name = "free", .replacement = reinterpret_cast<void *>(&hookFree), .required = true},
            ElfImportHookSpec{
                .name = "reallocarray", .replacement = reinterpret_cast<void *>(&hookReallocArray), .required = false},
            ElfImportHookSpec{
                .name = "aligned_alloc", .replacement = reinterpret_cast<void *>(&hookAlignedAlloc), .required = false},
            ElfImportHookSpec{.name = "posix_memalign",
                              .replacement = reinterpret_cast<void *>(&hookPosixMemalign),
                              .required = false},
        };
        if (!hooks.prepare(specs, error)) {
            return false;
        }
        updateHookCapabilities();

        if (!hooks.installed()) {
            cpptrace::frame_ptr warm[8]{};
            cpptrace::safe_generate_raw_trace(warm, 8, 0);
            if (warm[0] != 0) {
                cpptrace::safe_object_frame object;
                cpptrace::get_safe_object_frame(warm[0], &object);
            }
        }
        return true;
    }

    void updateHookCapabilities()
    {
        hook_capabilities.clear();
        for (const ElfImportHookCapability &capability : hooks.capabilities()) {
            hook_capabilities.push_back(
                {capability.name, capability.available ? AllocationHookStatus::Active : AllocationHookStatus::Missing,
                 capability.detail});
        }
    }

    bool installHooks(std::string &error)
    {
        if (hooks.installed()) {
            return true;
        }
        Impl *expected = nullptr;
        if (!mActiveInstance.compare_exchange_strong(expected, this, std::memory_order_release,
                                                     std::memory_order_relaxed) &&
            expected != this) {
            error = "another native allocation sampler backend is already active";
            return false;
        }
        if (!hooks.install(error)) {
            expected = this;
            mActiveInstance.compare_exchange_strong(expected, nullptr, std::memory_order_release,
                                                    std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    template <std::size_t N>
    static bool waitFor(const std::array<std::atomic<std::uint64_t>, N> &counters, const char *description,
                        std::string &error) noexcept
    {
        for (int attempt = 0; attempt < 5000; ++attempt) {
            bool quiescent = true;
            for (const auto &counter : counters) {
                if (counter.load(std::memory_order_acquire) != 0) {
                    quiescent = false;
                    break;
                }
            }
            if (quiescent) {
                return true;
            }
            timespec delay{.tv_sec = 0, .tv_nsec = 1000000};
            ::nanosleep(&delay, nullptr);
        }
        try {
            error = std::string("timed out waiting for ") + description + " to quiesce";
        }
        catch (...) {
            error.clear();
        }
        return false;
    }

    FrameKey frameKey(cpptrace::frame_ptr address)
    {
        cpptrace::safe_object_frame object;
        cpptrace::get_safe_object_frame(address, &object);
        std::string_view path =
            object.object_path[0] != '\0' ? std::string_view(object.object_path) : std::string_view("unknown");
        return aggregation.internFrame(path, static_cast<std::uint64_t>(object.address_relative_to_object_start),
                                       static_cast<std::uint64_t>(object.raw_address));
    }

    bool buildSample(const cpptrace::frame_ptr *frames, std::uint16_t depth, std::uint64_t tick_id,
                     std::uint64_t thread_id, std::uint64_t os_thread_id, std::int32_t window, std::uint64_t weight,
                     Sample &sample)
    {
        sample.tick_id = tick_id;
        const AllocationThreadSelection selection = aggregation.resolveThread(thread_id, os_thread_id);
        if (!selection.selected) {
            filtered_samples.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        sample.thread_id = selection.profile_thread_id;
        sample.os_thread_id = os_thread_id;
        sample.thread_name = selection.display_name;
        sample.window = window;
        sample.weight = weight;
        sample.frames.reserve(depth);
        for (std::size_t i = 0; i < depth; ++i) {
            if (frames[i] != 0) {
                sample.frames.push_back(frameKey(frames[i]));
            }
        }
        if (sample.frames.empty()) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    bool buildSnapshotSample(const cpptrace::frame_ptr *frames, std::uint16_t depth, std::uint64_t tick_id,
                             std::uint64_t thread_id, std::uint64_t os_thread_id, std::int32_t window,
                             std::uint64_t weight, Sample &sample)
    {
        sample.tick_id = tick_id;
        const AllocationThreadSelection selection = aggregation.resolveThread(thread_id, os_thread_id);
        if (!selection.selected) {
            return false;
        }
        sample.thread_id = selection.profile_thread_id;
        sample.os_thread_id = os_thread_id;
        sample.thread_name = selection.display_name;
        sample.window = window;
        sample.weight = weight;
        sample.frames.reserve(depth);
        for (std::size_t i = 0; i < depth; ++i) {
            if (frames[i] != 0) {
                sample.frames.push_back(frameKey(frames[i]));
            }
        }
        return !sample.frames.empty();
    }

    void processEvent(const AllocationEvent &event)
    {
        if (event.thread_observation) {
            aggregation.observeThread(event.thread_id, event.os_thread_id);
            return;
        }
        Sample sample;
        if (!buildSample(event.frames, event.depth, event.tick_id, event.thread_id, event.os_thread_id, event.window,
                         event.weight_bytes, sample)) {
            return;
        }
        (void)aggregation.processSample(std::move(sample));
    }

    void finalizeLiveProfile()
    {
        const std::uint64_t stopped_ms = monotonicMs();
        std::uint64_t total_age = 0;
        std::uint64_t maximum_age = 0;
        for (std::size_t i = 0; i < KLiveIndexCapacity; ++i) {
            const LiveIndexEntry &entry = live_index[i];
            void *pointer = entryPointer(entry);
            LiveAllocation *entry_allocation = entryAllocation(entry);
            if (pointer == nullptr || pointer == tombstonePointer() || entry_allocation == nullptr) {
                continue;
            }
            const LiveAllocation &allocation = *entry_allocation;
            if (!aggregation.tickAccepts(allocation.tick_id)) {
                continue;
            }
            Sample sample;
            if (buildSample(allocation.frames, allocation.depth, allocation.tick_id, allocation.thread_id,
                            allocation.os_thread_id, allocation.window, allocation.weight_bytes, sample)) {
                const std::uint64_t age =
                    stopped_ms >= allocation.allocated_ms ? stopped_ms - allocation.allocated_ms : 0;
                total_age += age;
                maximum_age = (std::max)(maximum_age, age);
                (void)aggregation.acceptLiveSample(std::move(sample));
            }
        }
        retained_age_ms_total.store(total_age, std::memory_order_relaxed);
        retained_age_ms_max.store(maximum_age, std::memory_order_relaxed);
    }

    void drainQueues()
    {
        TickEvent tick;
        while (ticks.dequeue(tick)) {
            aggregation.processTick(tick.tick_id, tick.mspt_ms);
        }
        AllocationEvent event;
        while (events.dequeue(event)) {
            processEvent(event);
        }
    }

    void aggregatorLoop()
    {
        TrackingSuppressionGuard suppress(*this);
        if (config.fail_aggregator_for_testing) {
            throw std::runtime_error("injected allocation aggregator failure");
        }
        if (config.hold_aggregator_until_event_drop_for_testing) {
            while (aggregator_running.load(std::memory_order_acquire) &&
                   dropped_events.load(std::memory_order_acquire) == 0) {
                std::this_thread::yield();
            }
        }
        if (config.aggregator_delay_ms_for_testing != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config.aggregator_delay_ms_for_testing));
        }
        while (aggregator_running.load(std::memory_order_acquire)) {
            {
                std::scoped_lock lock(aggregate_mutex);
                drainQueues();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::scoped_lock lock(aggregate_mutex);
            drainQueues();
            aggregation.finishPending();
        }
    }

    bool captureSnapshot(AllocationSnapshot &snapshot, std::string &error)
    {
        TrackingSuppressionGuard suppress(*this);
        std::scoped_lock lifecycle_lock(lifecycle_mutex);
        error.clear();
        if (!running.load(std::memory_order_acquire)) {
            return false;
        }
        if (aggregator_failed.load(std::memory_order_acquire)) {
            error = "allocation aggregator failed: " + std::string(aggregator_failure.data());
            return false;
        }

        std::unique_lock aggregate_lock(aggregate_mutex, std::defer_lock);
        if (!aggregate_lock.try_lock_for(std::chrono::seconds(5))) {
            error = "timed out waiting for the allocation aggregator snapshot";
            return false;
        }
        drainQueues();
        snapshot = AllocationSnapshot{};
        snapshot.number_of_ticks = current_tick.load(std::memory_order_relaxed);

        if (!config.live_only) {
            return aggregation.copyCumulativeSnapshot(snapshot, current_tick.load(std::memory_order_relaxed), error);
        }

        std::vector<LiveAllocation> retained;
        retained.reserve(
            (std::min)(KEventCapacity, static_cast<std::size_t>(live_samples.load(std::memory_order_relaxed))));
        const auto retained_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        lifecycle_readers.fetch_add(1, std::memory_order_acq_rel);
        bool stable = false;
        try {
            stable = detail::captureStableShardSnapshot(
                KLiveIndexShards, retained_deadline, [&retained] { retained.clear(); },
                [this] { return lifecycle_version.load(std::memory_order_acquire); },
                [this](std::size_t shard) { return ::pthread_rwlock_tryrdlock(&live_index_locks[shard]) == 0; },
                [this](std::size_t shard) { ::pthread_rwlock_unlock(&live_index_locks[shard]); },
                [this, &retained](std::size_t shard) {
                    const std::size_t begin = shard * KLiveIndexShardCapacity;
                    const std::size_t end = begin + KLiveIndexShardCapacity;
                    for (std::size_t i = begin; i < end; ++i) {
                        const LiveIndexEntry &entry = live_index[i];
                        void *pointer = entryPointer(entry);
                        LiveAllocation *entry_allocation = entryAllocation(entry);
                        if (pointer != nullptr && pointer != tombstonePointer() && entry_allocation != nullptr &&
                            entryAllocationId(entry) == entry_allocation->allocation_id) {
                            retained.push_back(*entry_allocation);
                        }
                    }
                },
                [] { return std::chrono::steady_clock::now(); }, [] { std::this_thread::yield(); });
        }
        catch (...) {
            lifecycle_readers.fetch_sub(1, std::memory_order_release);
            throw;
        }
        lifecycle_readers.fetch_sub(1, std::memory_order_release);
        if (!stable) {
            error = "timed out stabilizing retained allocation state";
            return false;
        }

        const std::uint64_t captured_ms = monotonicMs();
        std::vector<AllocationProfileAggregation::RetainedSample> prepared;
        prepared.reserve(retained.size());
        for (LiveAllocation &allocation : retained) {
            if (!aggregation.tickAccepts(allocation.tick_id)) {
                continue;
            }
            Sample sample;
            if (!buildSnapshotSample(allocation.frames, allocation.depth, allocation.tick_id, allocation.thread_id,
                                     allocation.os_thread_id, allocation.window, allocation.weight_bytes, sample)) {
                continue;
            }
            prepared.push_back(
                {.sample = std::move(sample),
                 .age_ms = captured_ms >= allocation.allocated_ms ? captured_ms - allocation.allocated_ms : 0});
        }
        return aggregation.buildLiveSnapshot(prepared, snapshot, current_tick.load(std::memory_order_relaxed), error);
    }

    bool setCurrentThreadTrackingSuppressed(bool suppressed) noexcept
    {
        ThreadSamplingState *state = currentThreadState();
        if (state == nullptr) {
            return false;
        }
        const bool previous = state->tracking_suppressed;
        state->tracking_suppressed = suppressed;
        return previous;
    }

    void markAggregatorFailure(const char *message) noexcept
    {
        tracking.store(false, std::memory_order_release);
        aggregator_running.store(false, std::memory_order_release);
        std::snprintf(aggregator_failure.data(), aggregator_failure.size(), "%s",
                      message != nullptr ? message : "unknown allocation aggregator failure");
        aggregator_failed.store(true, std::memory_order_release);
    }

    void resetSession()
    {
        TickEvent tick;
        while (ticks.dequeue(tick)) {
        }
        current_tick.store(0, std::memory_order_relaxed);
        for (auto &counters : hot_counters) {
            counters.hook_calls.store(0, std::memory_order_relaxed);
            counters.successful_allocation_calls.store(0, std::memory_order_relaxed);
            counters.observed_bytes.store(0, std::memory_order_relaxed);
        }
        sampling_points.store(0, std::memory_order_relaxed);
        filtered_samples.store(0, std::memory_order_relaxed);
        dropped_samples.store(0, std::memory_order_relaxed);
        dropped_events.store(0, std::memory_order_relaxed);
        dropped_tick_events.store(0, std::memory_order_relaxed);
        enqueued_samples.store(0, std::memory_order_relaxed);
        for (auto &counter : tracking_calls) {
            counter.store(0, std::memory_order_relaxed);
        }
        next_allocation_id.store(1, std::memory_order_relaxed);
        next_session_thread_id.store(1, std::memory_order_relaxed);
        registered_threads.store(0, std::memory_order_relaxed);
        overflow_threads.store(0, std::memory_order_relaxed);
        thread_state_drops.store(0, std::memory_order_relaxed);
        freed_samples.store(0, std::memory_order_relaxed);
        freed_bytes.store(0, std::memory_order_relaxed);
        live_samples.store(0, std::memory_order_relaxed);
        live_bytes.store(0, std::memory_order_relaxed);
        peak_live_samples.store(0, std::memory_order_relaxed);
        lifetime_ms_total.store(0, std::memory_order_relaxed);
        lifetime_ms_max.store(0, std::memory_order_relaxed);
        lifecycle_dropped.store(0, std::memory_order_relaxed);
        contention_dropped.store(0, std::memory_order_relaxed);
        lifecycle_version.store(0, std::memory_order_relaxed);
        lifecycle_readers.store(0, std::memory_order_relaxed);
        lifecycle_writers.store(0, std::memory_order_relaxed);
        for (auto &presence : live_presence) {
            presence.store(0, std::memory_order_relaxed);
        }
        deferred_live.store(nullptr, std::memory_order_relaxed);
        retained_age_ms_total.store(0, std::memory_order_relaxed);
        retained_age_ms_max.store(0, std::memory_order_relaxed);
        aggregator_failure.fill('\0');
        aggregator_failed.store(false, std::memory_order_release);
    }

    bool startSession(const AllocationSamplerConfig &new_config, std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();
        if (running.load(std::memory_order_acquire)) {
            error = "allocation profiler is already running";
            return false;
        }
        if (aggregator_thread.joinable()) {
            error = "the previous allocation session has not finished cleanup";
            return false;
        }
        if (new_config.session_seed == 0 || new_config.interval_bytes <= 0) {
            error = "invalid Linux allocation sampler configuration";
            return false;
        }
        resetSession();
        config = new_config;
        aggregation.reset(config, recovery_sink);
        if (!aggregation.configure(error)) {
            return false;
        }
        interval_bytes.store(static_cast<std::uint64_t>(new_config.interval_bytes), std::memory_order_relaxed);
        last_module_rescan_ms = monotonicMs();
        const std::uint64_t next_generation = generation.fetch_add(1, std::memory_order_relaxed) + 1;
        sampling_seed.store(next_generation ^ monotonicMs() ^ new_config.session_seed, std::memory_order_relaxed);

        if (!prepareHooks(error)) {
            return false;
        }
        if (!new_config.count_only && (!events.allocate(error) || !allocateLifecycleStorage(error))) {
            events.release();
            releaseLifecycleStorage();
            return false;
        }
        if (!installHooks(error)) {
            events.release();
            releaseLifecycleStorage();
            return false;
        }
        if (new_config.count_only) {
            running.store(true, std::memory_order_release);
            tracking.store(true, std::memory_order_release);
            return true;
        }

        aggregator_running.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        tracking.store(true, std::memory_order_release);
        try {
            TrackingSuppressionGuard suppress(*this);
            aggregator_thread = std::thread([this] {
                try {
                    aggregatorLoop();
                }
                catch (const std::exception &exception) {
                    markAggregatorFailure(exception.what());
                }
                catch (...) {
                    markAggregatorFailure("allocation aggregator failed with an unknown exception");
                }
            });
        }
        catch (...) {
            tracking.store(false, std::memory_order_release);
            running.store(false, std::memory_order_release);
            aggregator_running.store(false, std::memory_order_release);
            std::string quiescence_error;
            if (!waitFor(tracking_calls, "tracked Linux allocation hooks", quiescence_error)) {
                error = std::move(quiescence_error);
                return false;
            }
            events.release();
            releaseLifecycleStorage();
            error = "could not create the allocation aggregator thread";
            return false;
        }
        return true;
    }

    bool stopSession(std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();
        if (!running.load(std::memory_order_acquire) && !aggregator_thread.joinable()) {
            return true;
        }
        tracking.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (!waitFor(tracking_calls, "tracked Linux allocation hooks", error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregator_thread.joinable()) {
            aggregator_thread.join();
        }
        if (config.live_only && !aggregator_failed.load(std::memory_order_acquire)) {
            finalizeLiveProfile();
        }
        events.release();
        releaseLifecycleStorage();
        if (aggregator_failed.load(std::memory_order_acquire)) {
            error = "allocation aggregator failed: " + std::string(aggregator_failure.data());
            return false;
        }
        if (config.live_only && lifecycle_dropped.load(std::memory_order_relaxed) != 0) {
            error = "allocation lifecycle tracking capacity was exhausted; retained profile discarded";
            return false;
        }
        return true;
    }

    bool shutdownBackend(std::string &error)
    {
        std::scoped_lock lock(lifecycle_mutex);
        error.clear();
        tracking.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (!waitFor(tracking_calls, "tracked Linux allocation hooks", error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregator_thread.joinable()) {
            aggregator_thread.join();
        }
        events.release();
        releaseLifecycleStorage();

        if (hooks.installed() && !hooks.uninstall(error)) {
            return false;
        }
        if (!waitFor(mActiveHookCalls, "Linux allocation hook thunks", error)) {
            return false;
        }
        Impl *expected = this;
        mActiveInstance.compare_exchange_strong(expected, nullptr, std::memory_order_release,
                                                std::memory_order_relaxed);
        releaseThreadStateRegistry();
        return true;
    }

    void releaseThreadStateRegistry() noexcept
    {
        if (thread_state_key_created) {
            ::pthread_key_delete(thread_state_key);
            thread_state_key_created = false;
        }
        for (ThreadSamplingState &state : thread_states) {
            state.owner_tid.store(0, std::memory_order_relaxed);
            state.registry_state.store(0, std::memory_order_relaxed);
        }
    }

    void tick(double mspt_ms)
    {
        if (!running.load(std::memory_order_acquire) || aggregator_failed.load(std::memory_order_acquire)) {
            return;
        }
        TrackingSuppressionGuard suppress(*this);
        const std::uint64_t finished = current_tick.fetch_add(1, std::memory_order_relaxed);
        if (config.only_ticks_over_ms > 0 && !ticks.enqueue(TickEvent{.tick_id = finished, .mspt_ms = mspt_ms})) {
            dropped_tick_events.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t now = monotonicMs();
        if (now >= last_module_rescan_ms + 5000) {
            std::unique_lock lock(lifecycle_mutex, std::try_to_lock);
            if (lock.owns_lock()) {
                std::string ignored;
                if (hooks.rescan(ignored)) {
                    updateHookCapabilities();
                }
                last_module_rescan_ms = now;
            }
        }
        if (config.count_only) {
            return;
        }
        const std::int32_t window = profiling_window::windowNow();
        aggregation.recordTick(window, mspt_ms);
    }
};

std::atomic<AllocationSampler::Impl *> AllocationSampler::Impl::mActiveInstance{nullptr};
std::array<std::atomic<std::uint64_t>, KHookCallShards> AllocationSampler::Impl::mActiveHookCalls{};

AllocationSampler::AllocationSampler() : impl_(std::make_unique<Impl>()) {}

AllocationSampler::~AllocationSampler()
{
    if (impl_ == nullptr) {
        return;
    }
    std::string error;
    if (impl_->shutdownBackend(error)) {
        return;
    }
    if (!error.empty()) {
        std::fprintf(stderr, "[spark] Linux allocation sampler shutdown failed: %s\n", error.c_str());
    }
    std::abort();
}

bool AllocationSampler::start(const AllocationSamplerConfig &config, std::string &error)
{
    return impl_->startSession(config, error);
}

void AllocationSampler::setRecoverySink(RecoverySink *sink)
{
    impl_->recovery_sink = sink;
    impl_->aggregation.setRecoverySink(sink);
}

bool AllocationSampler::stop(std::string &error)
{
    return impl_->stopSession(error);
}

void AllocationSampler::requestStop() noexcept
{
    impl_->tracking.store(false, std::memory_order_release);
    impl_->running.store(false, std::memory_order_release);
}

bool AllocationSampler::shutdown(std::string &error)
{
    return impl_->shutdownBackend(error);
}
void AllocationSampler::onTick(double mspt_ms)
{
    impl_->tick(mspt_ms);
}

bool AllocationSampler::snapshot(AllocationSnapshot &snapshot, std::string &error)
{
    return impl_->captureSnapshot(snapshot, error);
}

bool AllocationSampler::setCurrentThreadTrackingSuppressed(bool suppressed) noexcept
{
    return impl_->setCurrentThreadTrackingSuppressed(suppressed);
}
const CallTree &AllocationSampler::tree() const
{
    return impl_->aggregation.tree();
}
const std::map<std::uint64_t, ThreadCallTree> &AllocationSampler::threadTrees() const
{
    return impl_->aggregation.threadTrees();
}
const ModuleTable &AllocationSampler::modules() const
{
    return impl_->aggregation.modules();
}
const std::map<std::int32_t, WindowTickStats> &AllocationSampler::windowTicks() const
{
    return impl_->aggregation.windowTicks();
}
std::uint64_t AllocationSampler::numberOfTicks() const
{
    return impl_->current_tick.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::hookCalls() const
{
    std::uint64_t total = 0;
    for (const auto &counters : impl_->hot_counters) {
        total += counters.hook_calls.load(std::memory_order_relaxed);
    }
    return total;
}
std::uint64_t AllocationSampler::successfulAllocationCalls() const
{
    std::uint64_t total = 0;
    for (const auto &counters : impl_->hot_counters) {
        total += counters.successful_allocation_calls.load(std::memory_order_relaxed);
    }
    return total;
}
std::uint64_t AllocationSampler::sampleCount() const
{
    return impl_->aggregation.sampleCount();
}
std::uint64_t AllocationSampler::samplingPoints() const
{
    return impl_->sampling_points.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::sampledBytes() const
{
    return impl_->aggregation.sampledBytes();
}
std::uint64_t AllocationSampler::filteredSamples() const
{
    return impl_->filtered_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::threadNameFailures() const
{
    return impl_->aggregation.threadNameFailures();
}
std::uint64_t AllocationSampler::threadIdentityCacheDrops() const
{
    return impl_->aggregation.threadIdentityCacheDrops();
}
std::uint64_t AllocationSampler::observedBytes() const
{
    std::uint64_t total = 0;
    for (const auto &counters : impl_->hot_counters) {
        total += counters.observed_bytes.load(std::memory_order_relaxed);
    }
    return total;
}
std::uint64_t AllocationSampler::droppedSamples() const
{
    return impl_->dropped_samples.load(std::memory_order_relaxed) + impl_->aggregation.droppedSamples();
}
std::uint64_t AllocationSampler::droppedEvents() const
{
    return impl_->dropped_events.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::droppedTickEvents() const
{
    return impl_->dropped_tick_events.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::tickEventCapacity()
{
    return KTickEventCapacity;
}
std::uint64_t AllocationSampler::enqueuedSamples() const
{
    return impl_->enqueued_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::eventQueueHighWaterMark() const
{
    return impl_->events.high_water.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::eventQueueCapacity()
{
    return KEventCapacity;
}
std::uint64_t AllocationSampler::freedSamples() const
{
    return impl_->freed_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::freedBytes() const
{
    return impl_->freed_bytes.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::liveSamples() const
{
    return impl_->live_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::liveBytes() const
{
    return impl_->live_bytes.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::peakLiveSamples() const
{
    return impl_->peak_live_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::liveIndexCapacity()
{
    return KEventCapacity;
}
std::uint64_t AllocationSampler::sampledThreadCount() const
{
    return impl_->aggregation.threadTrees().size();
}
std::uint64_t AllocationSampler::threadRootCapacity()
{
    return AllocationProfileAggregation::kThreadRootCapacity;
}
std::uint64_t AllocationSampler::overflowThreadCount() const
{
    return impl_->overflow_threads.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::threadStateDrops() const
{
    return impl_->thread_state_drops.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::hookedModuleCount() const
{
    return impl_->hooks.hookedModuleCount();
}
std::uint64_t AllocationSampler::skippedModuleCount() const
{
    return impl_->hooks.skippedModuleCount();
}
std::uint64_t AllocationSampler::failedModuleCount() const
{
    return impl_->hooks.failedModuleCount();
}
std::uint64_t AllocationSampler::moduleRegistryCount() const
{
    return impl_->aggregation.modules().size();
}
std::uint64_t AllocationSampler::moduleRegistryCapacity()
{
    return AllocationProfileAggregation::kModuleCapacity;
}
std::uint64_t AllocationSampler::profileNodeCapacity()
{
    return AllocationProfileAggregation::kProfileNodeCapacity;
}

std::uint64_t AllocationSampler::profileTimeEntryCapacity()
{
    return AllocationProfileAggregation::kProfileTimeEntryCapacity;
}

std::uint64_t AllocationSampler::profileStorageSampleDrops() const
{
    return impl_->aggregation.droppedProfileSamples();
}

bool AllocationSampler::profileStorageExhausted() const
{
    return impl_->aggregation.profileStorageExhausted();
}

std::uint64_t AllocationSampler::pendingSampleCapacity()
{
    return AllocationProfileAggregation::kPendingSampleCapacity;
}

std::uint64_t AllocationSampler::pendingSampleDrops() const
{
    return impl_->aggregation.pendingSampleDrops();
}

std::uint64_t AllocationSampler::pendingCapacityDrops() const
{
    return impl_->aggregation.pendingCapacityDrops();
}

std::uint64_t AllocationSampler::pendingStaleDrops() const
{
    return impl_->aggregation.pendingStaleDrops();
}

std::uint64_t AllocationSampler::pendingFinalDrops() const
{
    return impl_->aggregation.pendingFinalDrops();
}

std::uint64_t AllocationSampler::moduleOverflowFrames() const
{
    return impl_->aggregation.moduleOverflowFrames();
}

std::uint64_t AllocationSampler::retainedHistoryWindows() const
{
    return impl_->aggregation.retainedHistoryWindows();
}

std::uint64_t AllocationSampler::historySamplesPruned() const
{
    return impl_->aggregation.historySamplesPruned();
}

std::uint64_t AllocationSampler::historyBytesPruned() const
{
    return impl_->aggregation.historyBytesPruned();
}

bool AllocationSampler::historyTruncated() const
{
    return impl_->aggregation.historyTruncated();
}

bool AllocationSampler::dataIncomplete() const
{
    return impl_->dropped_samples.load(std::memory_order_relaxed) != 0 ||
           impl_->lifecycle_dropped.load(std::memory_order_relaxed) != 0 ||
           impl_->contention_dropped.load(std::memory_order_relaxed) != 0 ||
           impl_->dropped_tick_events.load(std::memory_order_relaxed) != 0 ||
           impl_->thread_state_drops.load(std::memory_order_relaxed) != 0 ||
           impl_->aggregation.threadIdentityCacheDrops() != 0 || impl_->aggregation.dataIncomplete() ||
           impl_->hooks.failedModuleCount() != 0;
}
std::uint64_t AllocationSampler::averageLifetimeMs() const
{
    const std::uint64_t count = impl_->freed_samples.load(std::memory_order_relaxed);
    return count == 0 ? 0 : impl_->lifetime_ms_total.load(std::memory_order_relaxed) / count;
}
std::uint64_t AllocationSampler::maximumLifetimeMs() const
{
    return impl_->lifetime_ms_max.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::lifecycleDropped() const
{
    return impl_->lifecycle_dropped.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::contentionDropped() const
{
    return impl_->contention_dropped.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::retainedAverageAgeMs() const
{
    const std::uint64_t count = impl_->aggregation.sampleCount();
    return count == 0 ? 0 : impl_->retained_age_ms_total.load(std::memory_order_relaxed) / count;
}
std::uint64_t AllocationSampler::retainedMaximumAgeMs() const
{
    return impl_->retained_age_ms_max.load(std::memory_order_relaxed);
}
bool AllocationSampler::running() const
{
    return impl_->running.load(std::memory_order_acquire);
}
bool AllocationSampler::hooksInstalled() const
{
    return impl_->hooks.installed();
}
bool AllocationSampler::failure(std::string &error) const
{
    if (!impl_->aggregator_failed.load(std::memory_order_acquire)) {
        error.clear();
        return false;
    }
    error = impl_->aggregator_failure.data();
    return true;
}
const std::vector<AllocationHookCapability> &AllocationSampler::hookCapabilities() const
{
    return impl_->hook_capabilities;
}
std::size_t AllocationSampler::hookTargetCount() const
{
    return impl_->hooks.targetCount();
}

}  // namespace spark
