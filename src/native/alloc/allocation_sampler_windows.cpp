#include "native/alloc/allocation_sampler.h"

#ifndef _WIN32
#error "allocation_sampler_windows.cpp must only be compiled on Windows"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows SDK headers require windows.h first
#include <windows.h>
#include <psapi.h>
#include <winternl.h>
// clang-format on

#include <funchook.h>

#include "native/alloc/allocation_profile_aggregation.h"
#include "native/alloc/bounded_event_queue.h"
#include "native/alloc/byte_sampler.h"
#include "native/alloc/stable_shard_snapshot.h"
#include "native/alloc/windows_thread_suspension.h"
#include "native/sampler/thread_info.h"
#include "profiling_window.h"

namespace spark {
namespace {

constexpr std::size_t KStackDepth = 48;
constexpr std::size_t KEventCapacity = 16384;
constexpr std::size_t KLiveIndexCapacity = KEventCapacity * 2;
constexpr std::size_t KLiveIndexShards = 64;
constexpr std::size_t KLiveIndexShardCapacity = KLiveIndexCapacity / KLiveIndexShards;
constexpr std::size_t KMaxSampledThreads = 256;
constexpr std::size_t KMaxThreadStates = 2048;
constexpr std::size_t KMaxAllocationModules = 512;
constexpr std::size_t KMaxModuleCacheEntries = 1024;
constexpr std::size_t KMaxProfileNodes = 131072;
constexpr std::size_t KMaxPendingSamples = 32768;
constexpr std::size_t KMaxTickDecisions = 100000;
constexpr std::size_t KTickEventCapacity = 4096;
constexpr std::size_t KHookPatchSize = 5;  // funchook 1.1.3 x86/x64 entry jump
constexpr std::uint32_t KFramesToSkip = 2;

void *tombstonePointer() noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(1));
}

struct PreparedTarget {
    void *address = nullptr;
    std::array<std::byte, KHookPatchSize> original{};
    std::string export_name;
};

std::uint64_t monotonicMs() noexcept
{
    return static_cast<std::uint64_t>(::GetTickCount64());
}

std::uint64_t saturatingMultiply(std::uint64_t a, std::uint64_t b) noexcept
{
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > max / b ? max : a * b;
}

bool checkedMultiply(std::size_t a, std::size_t b, std::uint64_t &out) noexcept
{
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        out = 0;
        return false;
    }
    out = static_cast<std::uint64_t>(a * b);
    return true;
}

std::string moduleBasename(const std::string &path)
{
    const std::size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool equalsIgnoreCase(const std::string &a, const char *b)
{
    return ::_stricmp(a.c_str(), b) == 0;
}

bool isLeadingAllocatorRuntime(const std::string &path)
{
    const std::string name = moduleBasename(path);
    return equalsIgnoreCase(name, "spark.dll") || equalsIgnoreCase(name, "ucrtbase.dll") ||
           equalsIgnoreCase(name, "vcruntime140.dll") || equalsIgnoreCase(name, "vcruntime140_1.dll") ||
           equalsIgnoreCase(name, "msvcp140.dll");
}

}  // namespace

struct AllocationSampler::Impl {
    using MallocFn = void *(__cdecl *)(std::size_t);
    using CallocFn = void *(__cdecl *)(std::size_t, std::size_t);
    using ReallocFn = void *(__cdecl *)(void *, std::size_t);
    using RecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t);
    using FreeFn = void(__cdecl *)(void *);
    using AlignedMallocFn = void *(__cdecl *)(std::size_t, std::size_t);
    using AlignedReallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t);
    using AlignedRecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t);
    using AlignedOffsetMallocFn = void *(__cdecl *)(std::size_t, std::size_t, std::size_t);
    using AlignedOffsetReallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t);
    using AlignedOffsetRecallocFn = void *(__cdecl *)(void *, std::size_t, std::size_t, std::size_t, std::size_t);
    using HeapAllocFn = void *(WINAPI *)(HANDLE, DWORD, SIZE_T);
    using HeapReAllocFn = void *(WINAPI *)(HANDLE, DWORD, void *, SIZE_T);
    using HeapFreeFn = BOOL(WINAPI *)(HANDLE, DWORD, void *);

    struct alignas(MEMORY_ALLOCATION_ALIGNMENT) AllocationEvent {
        SLIST_ENTRY entry{};
        std::uint64_t weight_bytes = 0;
        std::uint64_t tick_id = 0;
        std::uint64_t thread_id = 0;
        std::uint64_t os_thread_id = 0;
        std::int32_t window = 0;
        std::uint16_t depth = 0;
        bool thread_observation = false;
        void *frames[KStackDepth]{};
    };

    struct TickEvent {
        std::uint64_t tick_id = 0;
        double mspt_ms = 0.0;
    };

    struct alignas(MEMORY_ALLOCATION_ALIGNMENT) LiveAllocation {
        SLIST_ENTRY entry{};
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
        void *frames[KStackDepth]{};
    };

    struct LiveIndexEntry {
        void *pointer = nullptr;
        std::uint64_t allocation_id = 0;
        LiveAllocation *allocation = nullptr;
    };

    static_assert(alignof(AllocationEvent) >= MEMORY_ALLOCATION_ALIGNMENT);
    static_assert(sizeof(AllocationEvent) % MEMORY_ALLOCATION_ALIGNMENT == 0);
    static_assert(alignof(LiveAllocation) >= MEMORY_ALLOCATION_ALIGNMENT);
    static_assert(sizeof(LiveAllocation) % MEMORY_ALLOCATION_ALIGNMENT == 0);

    struct alignas(64) HookCounter {
        std::atomic<std::uint64_t> value{0};
    };

    static std::atomic<Impl *> mActiveInstance;
    static std::array<HookCounter, 64> mActiveHookCalls;

    struct ThreadSamplingState {
        // 0 free, 1 initializing, 2 active, 3 being checked for reclamation.
        std::atomic<std::uint8_t> registry_state{0};
        std::atomic<void *> teb{nullptr};
        HANDLE thread_handle = nullptr;
        ByteSamplingState bytes;
        std::uint64_t identity_generation = 0;
        std::uint64_t session_thread_id = 0;
        std::uint64_t os_thread_id = 0;
        bool inside_hook = false;
        bool tracking_suppressed = false;
        bool identity_announced = false;
    };

    class HookCallGuard {
    public:
        HookCallGuard() noexcept
        {
            const std::uintptr_t thread_key = reinterpret_cast<std::uintptr_t>(::NtCurrentTeb()) >> 12;
            counter_ = &mActiveHookCalls[static_cast<std::size_t>(thread_key) % mActiveHookCalls.size()];
            counter_->value.fetch_add(1, std::memory_order_acq_rel);
        }

        ~HookCallGuard() { counter_->value.fetch_sub(1, std::memory_order_release); }

        HookCallGuard(const HookCallGuard &) = delete;
        HookCallGuard &operator=(const HookCallGuard &) = delete;

    private:
        HookCounter *counter_ = nullptr;
    };

    class TrackingCallGuard {
    public:
        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl)
        {
            if (!impl_.tracking.load(std::memory_order_acquire)) {
                return;
            }
            impl_.tracking_hook_calls.fetch_add(1, std::memory_order_acq_rel);
            if (impl_.tracking.load(std::memory_order_acquire)) {
                active_ = true;
                return;
            }
            impl_.tracking_hook_calls.fetch_sub(1, std::memory_order_release);
        }

        ~TrackingCallGuard()
        {
            if (active_) {
                impl_.tracking_hook_calls.fetch_sub(1, std::memory_order_release);
            }
        }

        explicit operator bool() const noexcept { return active_; }

    private:
        Impl &impl_;
        bool active_ = false;
    };

    class RecursionGuard {
    public:
        explicit RecursionGuard(Impl &impl) noexcept : state_(impl.currentThreadState())
        {
            if (state_ != nullptr && !state_->inside_hook) {
                state_->inside_hook = true;
                owner_ = true;
            }
        }

        ~RecursionGuard()
        {
            if (owner_) {
                state_->inside_hook = false;
            }
        }

        [[nodiscard]] bool owner() const noexcept { return owner_; }

    private:
        ThreadSamplingState *state_ = nullptr;
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

    funchook_t *hooks = nullptr;
    bool hooks_prepared = false;
    bool hook_state_unknown = false;
    DWORD tls_index = TLS_OUT_OF_INDEXES;
    std::atomic<bool> hooks_installed{false};
    std::atomic<bool> tracking{false};
    std::atomic<bool> running{false};
    std::atomic<bool> aggregator_running{false};
    std::atomic<bool> aggregator_failed{false};
    std::array<char, 256> aggregator_failure{};

    MallocFn real_malloc = nullptr;
    CallocFn real_calloc = nullptr;
    ReallocFn real_realloc = nullptr;
    RecallocFn real_recalloc = nullptr;
    FreeFn real_free = nullptr;
    AlignedMallocFn real_aligned_malloc = nullptr;
    AlignedReallocFn real_aligned_realloc = nullptr;
    AlignedRecallocFn real_aligned_recalloc = nullptr;
    AlignedOffsetMallocFn real_aligned_offset_malloc = nullptr;
    AlignedOffsetReallocFn real_aligned_offset_realloc = nullptr;
    AlignedOffsetRecallocFn real_aligned_offset_recalloc = nullptr;
    FreeFn real_aligned_free = nullptr;

    // UCRT internal base exports are optional. Hooking them catches direct callers;
    // nested calls from public wrappers are suppressed by RecursionGuard.
    MallocFn real_malloc_base = nullptr;
    CallocFn real_calloc_base = nullptr;
    ReallocFn real_realloc_base = nullptr;
    FreeFn real_free_base = nullptr;
    HeapAllocFn real_heap_alloc = nullptr;
    HeapReAllocFn real_heap_realloc = nullptr;
    HeapFreeFn real_heap_free = nullptr;

    std::mutex lifecycle_mutex;
    std::timed_mutex aggregate_mutex;
    std::vector<PreparedTarget> prepared_targets;
    std::vector<WindowsCodeRange> protected_code_ranges;
    std::vector<AllocationHookCapability> hook_capabilities;
    AllocationSamplerConfig config{};
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> interval_bytes{kDefaultAllocationIntervalBytes};
    std::atomic<std::uint64_t> sampling_seed{0};
    std::atomic<std::uint64_t> hook_calls{0};
    std::atomic<std::uint64_t> successful_allocation_calls{0};
    std::atomic<std::uint64_t> sampling_points{0};
    std::atomic<std::uint64_t> filtered_samples{0};
    std::atomic<std::uint64_t> observed_bytes{0};
    std::atomic<std::uint64_t> dropped_samples{0};
    std::atomic<std::uint64_t> dropped_events{0};
    std::atomic<std::uint64_t> dropped_tick_events{0};
    std::atomic<std::uint64_t> enqueued_samples{0};
    std::atomic<std::uint64_t> ready_event_count{0};
    std::atomic<std::uint64_t> ready_event_high_water{0};
    std::atomic<std::uint64_t> tracking_hook_calls{0};
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
    SLIST_HEADER free_events{};
    SLIST_HEADER ready_events{};
    AllocationEvent *event_storage = nullptr;
    SLIST_HEADER free_live_allocations{};
    SLIST_HEADER deferred_live_allocations{};
    LiveAllocation *live_storage = nullptr;
    LiveIndexEntry *live_index = nullptr;
    std::array<SRWLOCK, KLiveIndexShards> live_index_locks{};
    std::array<ThreadSamplingState, KMaxThreadStates> thread_states{};

    std::thread aggregator_thread;
    BoundedEventQueue<TickEvent, KTickEventCapacity> ticks;

    AllocationProfileAggregation aggregation;
    std::unordered_map<std::uintptr_t, ModuleId> module_cache;

    RecoverySink *recovery_sink = nullptr;

    ~Impl() = default;

    static Impl *active() noexcept { return mActiveInstance.load(std::memory_order_acquire); }

    static Impl *activeOrAbort() noexcept
    {
        Impl *self = active();
        if (self == nullptr) {
            std::abort();
        }
        if (self->tracking.load(std::memory_order_relaxed)) {
            self->hook_calls.fetch_add(1, std::memory_order_relaxed);
        }
        return self;
    }

    static void *__cdecl hookMalloc(std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleMalloc(self->real_malloc, size);
    }

    static void *__cdecl hookCalloc(std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleCalloc(self->real_calloc, count, size);
    }

    static void *__cdecl hookRealloc(void *pointer, std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleRealloc(self->real_realloc, pointer, size);
    }

    static void *__cdecl hookRecalloc(void *pointer, std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleRecalloc(self->real_recalloc, pointer, count, size);
    }

    static void __cdecl hookFree(void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        self->handleFree(self->real_free, pointer);
    }

    static void *__cdecl hookAlignedMalloc(std::size_t size, std::size_t alignment) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedMalloc(self->real_aligned_malloc, size, alignment);
    }

    static void *__cdecl hookAlignedRealloc(void *pointer, std::size_t size, std::size_t alignment) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedRealloc(self->real_aligned_realloc, pointer, size, alignment);
    }

    static void *__cdecl hookAlignedRecalloc(void *pointer, std::size_t count, std::size_t size,
                                             std::size_t alignment) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedRecalloc(self->real_aligned_recalloc, pointer, count, size, alignment);
    }

    static void *__cdecl hookAlignedOffsetMalloc(std::size_t size, std::size_t alignment, std::size_t offset) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetMalloc(self->real_aligned_offset_malloc, size, alignment, offset);
    }

    static void *__cdecl hookAlignedOffsetRealloc(void *pointer, std::size_t size, std::size_t alignment,
                                                  std::size_t offset) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetRealloc(self->real_aligned_offset_realloc, pointer, size, alignment, offset);
    }

    static void *__cdecl hookAlignedOffsetRecalloc(void *pointer, std::size_t count, std::size_t size,
                                                   std::size_t alignment, std::size_t offset) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleAlignedOffsetRecalloc(self->real_aligned_offset_recalloc, pointer, count, size, alignment,
                                                 offset);
    }

    static void __cdecl hookAlignedFree(void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        self->handleFree(self->real_aligned_free, pointer);
    }

    static void *__cdecl hookMallocBase(std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleMalloc(self->real_malloc_base, size);
    }

    static void *__cdecl hookCallocBase(std::size_t count, std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleCalloc(self->real_calloc_base, count, size);
    }

    static void *__cdecl hookReallocBase(void *pointer, std::size_t size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleRealloc(self->real_realloc_base, pointer, size);
    }

    static void __cdecl hookFreeBase(void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        self->handleFree(self->real_free_base, pointer);
    }

    static void *WINAPI hookHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleHeapAlloc(self->real_heap_alloc, heap, flags, size);
    }

    static void *WINAPI hookHeapReAlloc(HANDLE heap, DWORD flags, void *pointer, SIZE_T size) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleHeapReAlloc(self->real_heap_realloc, heap, flags, pointer, size);
    }

    static BOOL WINAPI hookHeapFree(HANDLE heap, DWORD flags, void *pointer) noexcept
    {
        HookCallGuard activity;
        Impl *self = activeOrAbort();
        return self->handleHeapFree(self->real_heap_free, heap, flags, pointer);
    }

    ThreadSamplingState *currentThreadState() noexcept
    {
        if (tls_index == TLS_OUT_OF_INDEXES) {
            return nullptr;
        }

        PTEB teb = ::NtCurrentTeb();
        // Reserved1[11] is the TEB ThreadLocalStoragePointer slot; null while TLS is being initialized.
        if (teb == nullptr || teb->Reserved1[11] == nullptr) {
            return nullptr;
        }

        void *value = ::TlsGetValue(tls_index);
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
        if (address >= begin && address < end && state->registry_state.load(std::memory_order_acquire) != 0 &&
            state->teb.load(std::memory_order_acquire) == teb) {
            return state;
        }

        for (std::size_t i = 0; i < state_limit; ++i) {
            ThreadSamplingState &candidate = thread_states[i];
            if (candidate.registry_state.load(std::memory_order_acquire) == 1 &&
                candidate.teb.load(std::memory_order_acquire) == teb) {
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
            expected = 2;
            if (candidate.registry_state.compare_exchange_strong(expected, 3, std::memory_order_acq_rel)) {
                if (candidate.thread_handle != nullptr &&
                    ::WaitForSingleObject(candidate.thread_handle, 0) == WAIT_OBJECT_0) {
                    ::CloseHandle(candidate.thread_handle);
                    candidate.thread_handle = nullptr;
                    candidate.registry_state.store(1, std::memory_order_release);
                    claimed = &candidate;
                    break;
                }
                candidate.registry_state.store(2, std::memory_order_release);
            }
        }
        if (claimed == nullptr) {
            thread_state_drops.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        claimed->teb.store(teb, std::memory_order_release);
        claimed->bytes = {};
        claimed->identity_generation = 0;
        claimed->session_thread_id = 0;
        claimed->os_thread_id = static_cast<std::uint64_t>(::GetCurrentThreadId());
        claimed->inside_hook = true;
        claimed->tracking_suppressed = false;
        claimed->identity_announced = false;
        HANDLE thread_handle = nullptr;
        if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &thread_handle,
                              SYNCHRONIZE, FALSE, 0) != FALSE) {
            claimed->thread_handle = thread_handle;
        }
        if (::TlsSetValue(tls_index, claimed) == FALSE) {
            if (claimed->thread_handle != nullptr) {
                ::CloseHandle(claimed->thread_handle);
                claimed->thread_handle = nullptr;
            }
            claimed->inside_hook = false;
            claimed->teb.store(nullptr, std::memory_order_release);
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
        auto *self = const_cast<Impl *>(this);
        ThreadSamplingState *state = self->currentThreadState();
        return state != nullptr && !state->tracking_suppressed;
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

    void recycleLiveRecord(LiveAllocation *allocation) noexcept
    {
        SLIST_HEADER *destination = lifecycle_readers.load(std::memory_order_acquire) == 0 ? &free_live_allocations
                                                                                           : &deferred_live_allocations;
        ::InterlockedPushEntrySList(destination, &allocation->entry);
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
        std::uint64_t previous_max = lifetime_ms_max.load(std::memory_order_relaxed);
        while (previous_max < lifetime &&
               !lifetime_ms_max.compare_exchange_weak(previous_max, lifetime, std::memory_order_relaxed)) {
        }
        recycleLiveRecord(allocation);
    }

    LiveAllocation *detachAllocation(void *pointer) noexcept
    {
        if (pointer == nullptr || live_index == nullptr) {
            return nullptr;
        }
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t shard = liveIndexShard(hash);
        if (config.force_live_lock_contention_for_testing || !::TryAcquireSRWLockExclusive(&live_index_locks[shard])) {
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
                break;
            }
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);
        lifecycle_writers.fetch_sub(1, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&live_index_locks[shard]);
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

    void handleFree(FreeFn function, void *pointer) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            function(pointer);
            return;
        }
        TrackingCallGuard tracking_call(*this);
        RecursionGuard recursion(*this);
        if (!tracking_call || !recursion.owner()) {
            function(pointer);
            return;
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        function(pointer);
        if (allocation != nullptr) {
            retireAllocation(allocation, monotonicMs());
        }
    }

    BOOL handleHeapFree(HeapFreeFn function, HANDLE heap, DWORD flags, void *pointer) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(heap, flags, pointer);
        }
        TrackingCallGuard tracking_call(*this);
        RecursionGuard recursion(*this);
        if (!tracking_call || !recursion.owner()) {
            return function(heap, flags, pointer);
        }
        LiveAllocation *allocation = detachAllocation(pointer);
        const BOOL result = function(heap, flags, pointer);
        if (result != FALSE) {
            if (allocation != nullptr) {
                retireAllocation(allocation, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(allocation);
        }
        return result;
    }

    void *handleMalloc(MallocFn function, std::size_t requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(requested_size);
        }
        void *pointer = function(requested_size);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(requested_size));
        }
        return pointer;
    }

    void *handleCalloc(CallocFn function, std::size_t count, std::size_t requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(count, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(count, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(count, requested_size);
        }
        void *pointer = function(count, requested_size);
        if (pointer != nullptr) {
            std::uint64_t bytes = 0;
            if (checkedMultiply(count, requested_size, bytes)) {
                recordAllocation(pointer, bytes);
            }
        }
        return pointer;
    }

    void *handleRealloc(ReallocFn function, void *pointer, std::size_t requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, requested_size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, requested_size);
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && requested_size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && requested_size != 0) {
            // Allocation weights use successful requested bytes, not usable heap size.
            recordAllocation(new_pointer, static_cast<std::uint64_t>(requested_size));
        }
        return new_pointer;
    }

    void *handleRecalloc(RecallocFn function, void *pointer, std::size_t count, std::size_t requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, count, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, count, requested_size);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, requested_size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, count, requested_size);
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && valid_size && bytes != 0) {
            recordAllocation(new_pointer, bytes);
        }
        return new_pointer;
    }

    void *handleAlignedMalloc(AlignedMallocFn function, std::size_t size, std::size_t alignment) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(size, alignment);
        }
        void *pointer = function(size, alignment);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(size));
        }
        return pointer;
    }

    void *handleAlignedRealloc(AlignedReallocFn function, void *pointer, std::size_t size,
                               std::size_t alignment) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, size, alignment);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, size, alignment);
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && size != 0) {
            recordAllocation(new_pointer, static_cast<std::uint64_t>(size));
        }
        return new_pointer;
    }

    void *handleAlignedRecalloc(AlignedRecallocFn function, void *pointer, std::size_t count, std::size_t size,
                                std::size_t alignment) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, count, size, alignment);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, size, alignment);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, count, size, alignment);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, count, size, alignment);
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && valid_size && bytes != 0) {
            recordAllocation(new_pointer, bytes);
        }
        return new_pointer;
    }

    void *handleAlignedOffsetMalloc(AlignedOffsetMallocFn function, std::size_t size, std::size_t alignment,
                                    std::size_t offset) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(size, alignment, offset);
        }
        void *pointer = function(size, alignment, offset);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(size));
        }
        return pointer;
    }

    void *handleAlignedOffsetRealloc(AlignedOffsetReallocFn function, void *pointer, std::size_t size,
                                     std::size_t alignment, std::size_t offset) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, size, alignment, offset);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, size, alignment, offset);
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && size == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && size != 0) {
            recordAllocation(new_pointer, static_cast<std::uint64_t>(size));
        }
        return new_pointer;
    }

    void *handleAlignedOffsetRecalloc(AlignedOffsetRecallocFn function, void *pointer, std::size_t count,
                                      std::size_t size, std::size_t alignment, std::size_t offset) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(pointer, count, size, alignment, offset);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(pointer, count, size, alignment, offset);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(pointer, count, size, alignment, offset);
        }
        std::uint64_t bytes = 0;
        const bool valid_size = checkedMultiply(count, size, bytes);
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(pointer, count, size, alignment, offset);
        const bool replaced = new_pointer != nullptr || (pointer != nullptr && valid_size && bytes == 0);
        if (replaced) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && valid_size && bytes != 0) {
            recordAllocation(new_pointer, bytes);
        }
        return new_pointer;
    }

    void *handleHeapAlloc(HeapAllocFn function, HANDLE heap, DWORD flags, SIZE_T requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(heap, flags, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(heap, flags, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(heap, flags, requested_size);
        }
        void *pointer = function(heap, flags, requested_size);
        if (pointer != nullptr) {
            recordAllocation(pointer, static_cast<std::uint64_t>(requested_size));
        }
        return pointer;
    }

    void *handleHeapReAlloc(HeapReAllocFn function, HANDLE heap, DWORD flags, void *pointer,
                            SIZE_T requested_size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return function(heap, flags, pointer, requested_size);
        }
        TrackingCallGuard tracking_call(*this);
        if (!tracking_call) {
            return function(heap, flags, pointer, requested_size);
        }
        RecursionGuard recursion(*this);
        if (!recursion.owner()) {
            return function(heap, flags, pointer, requested_size);
        }
        LiveAllocation *previous = detachAllocation(pointer);
        void *new_pointer = function(heap, flags, pointer, requested_size);
        if (new_pointer != nullptr) {
            if (previous != nullptr) {
                retireAllocation(previous, monotonicMs());
            }
        }
        else {
            restoreDetachedAllocation(previous);
        }
        if (new_pointer != nullptr && requested_size != 0) {
            recordAllocation(new_pointer, static_cast<std::uint64_t>(requested_size));
        }
        return new_pointer;
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
        LiveAllocation *replaced = nullptr;
        bool inserted = false;
        void *pointer = allocation->pointer;
        const std::uint64_t allocation_id = allocation->allocation_id;
        const std::uint64_t weight = allocation->weight_bytes;
        const std::uint64_t hash = liveIndexHash(pointer);
        const std::size_t shard = liveIndexShard(hash);
        if (config.force_live_lock_contention_for_testing || !::TryAcquireSRWLockExclusive(&live_index_locks[shard])) {
            contention_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        lifecycle_writers.fetch_add(1, std::memory_order_acq_rel);
        lifecycle_version.fetch_add(1, std::memory_order_release);
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
                break;
            }
        }
        if (!inserted && tombstone != KLiveIndexCapacity) {
            if (account_live) {
                accountLiveAllocation(weight);
            }
            publishEntry(live_index[tombstone], pointer, allocation_id, allocation);
            inserted = true;
        }
        lifecycle_version.fetch_add(1, std::memory_order_release);
        lifecycle_writers.fetch_sub(1, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&live_index_locks[shard]);

        if (replaced != nullptr) {
            // Pointer reuse retires lifecycle records missed by covered frees.
            retireAllocation(replaced, monotonicMs());
        }
        return inserted;
    }

    void recordAllocation(void *pointer, std::uint64_t requested_bytes) noexcept
    {
        successful_allocation_calls.fetch_add(1, std::memory_order_relaxed);
        if (requested_bytes == 0) {
            return;
        }
        observed_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);

        ThreadSamplingState *thread_pointer = currentThreadState();
        if (thread_pointer == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ThreadSamplingState &thread = *thread_pointer;
        ByteSamplingState &state = thread.bytes;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        const auto current_tid = static_cast<std::uint64_t>(::GetCurrentThreadId());
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

        const std::uint64_t sample_points = consumeSampledBytes(state, requested_bytes, interval);
        if (sample_points == 0) {
            return;
        }
        sampling_points.fetch_add(sample_points, std::memory_order_relaxed);

        // Each sampling point contributes one configured interval.
        const std::uint64_t weight = saturatingMultiply(sample_points, interval);

        PSLIST_ENTRY live_entry = ::InterlockedPopEntrySList(&free_live_allocations);
        if (live_entry == nullptr && lifecycle_readers.load(std::memory_order_acquire) == 0) {
            live_entry = ::InterlockedPopEntrySList(&deferred_live_allocations);
        }
        if (live_entry == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            lifecycle_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto *allocation = CONTAINING_RECORD(live_entry, LiveAllocation, entry);
        allocation->pointer = pointer;
        allocation->allocation_id = next_allocation_id.fetch_add(1, std::memory_order_relaxed);
        allocation->weight_bytes = weight;
        allocation->requested_bytes = requested_bytes;
        allocation->allocated_ms = monotonicMs();
        allocation->tick_id = current_tick.load(std::memory_order_relaxed);
        allocation->thread_id = thread.session_thread_id;
        allocation->os_thread_id = thread.os_thread_id;
        allocation->window = profiling_window::windowNow();
        allocation->depth = static_cast<std::uint16_t>(
            ::RtlCaptureStackBackTrace(KFramesToSkip, static_cast<ULONG>(KStackDepth), allocation->frames, nullptr));
        const std::uint64_t allocation_weight = allocation->weight_bytes;
        const std::uint64_t allocation_tick = allocation->tick_id;
        const std::uint64_t allocation_thread = allocation->thread_id;
        const std::uint64_t allocation_os_thread = allocation->os_thread_id;
        const std::int32_t allocation_window = allocation->window;
        const std::uint16_t allocation_depth = allocation->depth;
        const bool live_only = config.live_only;
        AllocationEvent snapshot{};
        snapshot.thread_id = allocation_thread;
        snapshot.os_thread_id = allocation_os_thread;
        snapshot.thread_observation = live_only;
        if (!live_only) {
            snapshot.weight_bytes = allocation_weight;
            snapshot.tick_id = allocation_tick;
            snapshot.window = allocation_window;
            snapshot.depth = allocation_depth;
            std::memcpy(static_cast<void *>(snapshot.frames), static_cast<const void *>(allocation->frames),
                        static_cast<std::size_t>(allocation_depth) * sizeof(void *));
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
                PSLIST_ENTRY observation_entry = ::InterlockedPopEntrySList(&free_events);
                if (observation_entry == nullptr) {
                    dropped_samples.fetch_add(1, std::memory_order_relaxed);
                    dropped_events.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    auto *observation = CONTAINING_RECORD(observation_entry, AllocationEvent, entry);
                    observation->thread_id = snapshot.thread_id;
                    observation->os_thread_id = snapshot.os_thread_id;
                    observation->depth = 0;
                    observation->thread_observation = true;
                    const std::uint64_t ready = ready_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    std::uint64_t previous_high_water = ready_event_high_water.load(std::memory_order_relaxed);
                    while (previous_high_water < ready && !ready_event_high_water.compare_exchange_weak(
                                                              previous_high_water, ready, std::memory_order_relaxed)) {
                    }
                    ::InterlockedPushEntrySList(&ready_events, &observation->entry);
                    thread.identity_announced = true;
                }
            }
            return;
        }

        PSLIST_ENTRY entry = ::InterlockedPopEntrySList(&free_events);
        if (entry == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            dropped_events.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto *event = CONTAINING_RECORD(entry, AllocationEvent, entry);
        event->weight_bytes = snapshot.weight_bytes;
        event->tick_id = snapshot.tick_id;
        event->thread_id = snapshot.thread_id;
        event->os_thread_id = snapshot.os_thread_id;
        event->window = snapshot.window;
        event->depth = snapshot.depth;
        event->thread_observation = snapshot.thread_observation;
        std::memcpy(static_cast<void *>(event->frames), static_cast<const void *>(snapshot.frames),
                    static_cast<std::size_t>(snapshot.depth) * sizeof(void *));
        const std::uint64_t ready = ready_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t previous_high_water = ready_event_high_water.load(std::memory_order_relaxed);
        while (previous_high_water < ready &&
               !ready_event_high_water.compare_exchange_weak(previous_high_water, ready, std::memory_order_relaxed)) {
        }
        ::InterlockedPushEntrySList(&ready_events, &event->entry);
        enqueued_samples.fetch_add(1, std::memory_order_relaxed);
    }

    bool allocateEventPool(std::string &error)
    {
        ::InitializeSListHead(&free_events);
        ::InitializeSListHead(&ready_events);
        ::InitializeSListHead(&free_live_allocations);
        ::InitializeSListHead(&deferred_live_allocations);
        const std::size_t bytes = sizeof(AllocationEvent) * KEventCapacity;
        event_storage =
            static_cast<AllocationEvent *>(::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (event_storage == nullptr) {
            error = "VirtualAlloc for allocation sample buffer failed: " + std::to_string(::GetLastError());
            return false;
        }
        live_storage = static_cast<LiveAllocation *>(
            ::VirtualAlloc(nullptr, sizeof(LiveAllocation) * KEventCapacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        live_index = static_cast<LiveIndexEntry *>(::VirtualAlloc(nullptr, sizeof(LiveIndexEntry) * KLiveIndexCapacity,
                                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (live_storage == nullptr || live_index == nullptr) {
            error = "VirtualAlloc for allocation lifecycle tracking failed: " + std::to_string(::GetLastError());
            freeEventPool();
            return false;
        }
        for (std::size_t i = 0; i < KEventCapacity; ++i) {
            ::new (static_cast<void *>(&event_storage[i])) AllocationEvent{};
            ::InterlockedPushEntrySList(&free_events, &event_storage[i].entry);
            ::new (static_cast<void *>(&live_storage[i])) LiveAllocation{};
            ::InterlockedPushEntrySList(&free_live_allocations, &live_storage[i].entry);
        }
        return true;
    }

    void freeEventPool() noexcept
    {
        if (event_storage != nullptr) {
            ::VirtualFree(event_storage, 0, MEM_RELEASE);
            event_storage = nullptr;
        }
        if (live_storage != nullptr) {
            ::VirtualFree(live_storage, 0, MEM_RELEASE);
            live_storage = nullptr;
        }
        if (live_index != nullptr) {
            ::VirtualFree(live_index, 0, MEM_RELEASE);
            live_index = nullptr;
        }
        ::InitializeSListHead(&free_events);
        ::InitializeSListHead(&ready_events);
        ::InitializeSListHead(&free_live_allocations);
        ::InitializeSListHead(&deferred_live_allocations);
    }

    void recycleEvent(AllocationEvent *event) noexcept { ::InterlockedPushEntrySList(&free_events, &event->entry); }

    std::string hookError(const char *operation, int code) const
    {
        std::string message(operation);
        message += " failed (code ";
        message += std::to_string(code);
        message += ")";
        if (hooks != nullptr) {
            const char *detail = funchook_error_message(hooks);
            if (detail != nullptr && *detail != '\0') {
                message += ": ";
                message += detail;
            }
        }
        return message;
    }

    template <typename Function>
    bool prepareExport(HMODULE module, const char *name, Function &function, void *hook, bool required,
                       std::string &error)
    {
        void *target = reinterpret_cast<void *>(::GetProcAddress(module, name));
        if (target == nullptr) {
            function = nullptr;
            hook_capabilities.push_back(
                {.name = name, .status = AllocationHookStatus::Missing, .detail = "export not found"});
            if (required) {
                error = std::string("required UCRT allocation export not found: ") + name;
                return false;
            }
            return true;
        }
        auto alias = std::find_if(prepared_targets.begin(), prepared_targets.end(),
                                  [target](const PreparedTarget &entry) { return entry.address == target; });
        if (alias != prepared_targets.end()) {
            // Some CRT exports are aliases for the same entry address. The first
            // prepared hook already covers all aliases; preparing the same prologue
            // twice would create an invalid hook chain.
            function = nullptr;
            hook_capabilities.push_back({name, AllocationHookStatus::Alias, alias->export_name});
            return true;
        }

        PreparedTarget prepared;
        prepared.address = target;
        prepared.export_name = name;
        std::memcpy(prepared.original.data(), target, prepared.original.size());
        function = reinterpret_cast<Function>(target);
        const int code = funchook_prepare(hooks, reinterpret_cast<void **>(&function), hook);
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            const std::string failure = hookError((std::string("funchook_prepare(") + name + ")").c_str(), code);
            if (!required) {
                function = nullptr;
                hook_capabilities.push_back(
                    {.name = name, .status = AllocationHookStatus::PrepareFailed, .detail = failure});
                return true;
            }
            hook_capabilities.push_back(
                {.name = name, .status = AllocationHookStatus::PrepareFailed, .detail = failure});
            error = failure;
            return false;
        }
        prepared_targets.push_back(prepared);
        hook_capabilities.push_back({.name = name, .status = AllocationHookStatus::Active, .detail = {}});
        return true;
    }

    void addProtectedCodeRange(void *address)
    {
        if (address == nullptr) {
            return;
        }
        MEMORY_BASIC_INFORMATION memory{};
        if (::VirtualQuery(address, &memory, sizeof(memory)) == 0) {
            return;
        }
        WindowsCodeRange range;
        range.begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        range.end = range.begin + memory.RegionSize;
        if (std::ranges::none_of(protected_code_ranges, [&range](const WindowsCodeRange &existing) {
                return existing.begin == range.begin && existing.end == range.end;
            })) {
            protected_code_ranges.push_back(range);
        }
    }

    bool rebuildProtectedCodeRanges(std::string &error)
    {
        protected_code_ranges.clear();

        HMODULE module = nullptr;
        const auto *const hook_address = reinterpret_cast<LPCWSTR>(&AllocationSampler::Impl::hookMalloc);
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 hook_address, &module) == FALSE) {
            error = "GetModuleHandleExW for Spark hook module failed: " + std::to_string(::GetLastError());
            return false;
        }
        MODULEINFO module_info{};
        if (::GetModuleInformation(::GetCurrentProcess(), module, &module_info, sizeof(module_info)) == FALSE) {
            error = "GetModuleInformation for Spark hook module failed: " + std::to_string(::GetLastError());
            return false;
        }
        protected_code_ranges.push_back(
            {.begin = reinterpret_cast<std::uintptr_t>(module_info.lpBaseOfDll),
             .end = reinterpret_cast<std::uintptr_t>(module_info.lpBaseOfDll) + module_info.SizeOfImage});

        for (void *address : {
                 reinterpret_cast<void *>(real_malloc),
                 reinterpret_cast<void *>(real_calloc),
                 reinterpret_cast<void *>(real_realloc),
                 reinterpret_cast<void *>(real_recalloc),
                 reinterpret_cast<void *>(real_free),
                 reinterpret_cast<void *>(real_aligned_malloc),
                 reinterpret_cast<void *>(real_aligned_realloc),
                 reinterpret_cast<void *>(real_aligned_recalloc),
                 reinterpret_cast<void *>(real_aligned_offset_malloc),
                 reinterpret_cast<void *>(real_aligned_offset_realloc),
                 reinterpret_cast<void *>(real_aligned_offset_recalloc),
                 reinterpret_cast<void *>(real_aligned_free),
                 reinterpret_cast<void *>(real_malloc_base),
                 reinterpret_cast<void *>(real_calloc_base),
                 reinterpret_cast<void *>(real_realloc_base),
                 reinterpret_cast<void *>(real_free_base),
                 reinterpret_cast<void *>(real_heap_alloc),
                 reinterpret_cast<void *>(real_heap_realloc),
                 reinterpret_cast<void *>(real_heap_free),
             }) {
            addProtectedCodeRange(address);
        }
        return true;
    }

    bool restoreOriginalTargets(DWORD &first_failure) noexcept
    {
        first_failure = ERROR_SUCCESS;
        for (const PreparedTarget &target : prepared_targets) {
            DWORD old_protection = 0;
            if (::VirtualProtect(target.address, target.original.size(), PAGE_EXECUTE_READWRITE, &old_protection) ==
                FALSE) {
                if (first_failure == ERROR_SUCCESS) {
                    first_failure = ::GetLastError();
                }
                continue;
            }
            std::memcpy(target.address, target.original.data(), target.original.size());
            ::FlushInstructionCache(::GetCurrentProcess(), target.address, target.original.size());
            DWORD ignored = 0;
            if (::VirtualProtect(target.address, target.original.size(), old_protection, &ignored) == FALSE &&
                first_failure == ERROR_SUCCESS) {
                first_failure = ::GetLastError();
            }
        }
        return first_failure == ERROR_SUCCESS;
    }

    bool prepareHooks(std::string &error)
    {
        if (hooks_prepared) {
            return true;
        }
        if (tls_index == TLS_OUT_OF_INDEXES) {
            tls_index = ::TlsAlloc();
            if (tls_index == TLS_OUT_OF_INDEXES) {
                error = "TlsAlloc for allocation thread state failed: " + std::to_string(::GetLastError());
                return false;
            }
        }

        HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
        if (ucrt == nullptr) {
            error = "ucrtbase.dll is not loaded";
            return false;
        }
        HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        if (kernel32 == nullptr) {
            error = "kernel32.dll is not loaded";
            return false;
        }

        hooks = funchook_create();
        if (hooks == nullptr) {
            error = "funchook_create failed";
            return false;
        }

        bool ok =
            prepareExport(ucrt, "malloc", real_malloc, reinterpret_cast<void *>(&hookMalloc), true, error) &&
            prepareExport(ucrt, "calloc", real_calloc, reinterpret_cast<void *>(&hookCalloc), true, error) &&
            prepareExport(ucrt, "realloc", real_realloc, reinterpret_cast<void *>(&hookRealloc), true, error) &&
            prepareExport(ucrt, "_recalloc", real_recalloc, reinterpret_cast<void *>(&hookRecalloc), false, error) &&
            prepareExport(ucrt, "free", real_free, reinterpret_cast<void *>(&hookFree), true, error) &&
            prepareExport(ucrt, "_aligned_malloc", real_aligned_malloc, reinterpret_cast<void *>(&hookAlignedMalloc),
                          false, error) &&
            prepareExport(ucrt, "_aligned_realloc", real_aligned_realloc, reinterpret_cast<void *>(&hookAlignedRealloc),
                          false, error) &&
            prepareExport(ucrt, "_aligned_recalloc", real_aligned_recalloc,
                          reinterpret_cast<void *>(&hookAlignedRecalloc), false, error) &&
            prepareExport(ucrt, "_aligned_offset_malloc", real_aligned_offset_malloc,
                          reinterpret_cast<void *>(&hookAlignedOffsetMalloc), false, error) &&
            prepareExport(ucrt, "_aligned_offset_realloc", real_aligned_offset_realloc,
                          reinterpret_cast<void *>(&hookAlignedOffsetRealloc), false, error) &&
            prepareExport(ucrt, "_aligned_offset_recalloc", real_aligned_offset_recalloc,
                          reinterpret_cast<void *>(&hookAlignedOffsetRecalloc), false, error) &&
            prepareExport(ucrt, "_aligned_free", real_aligned_free, reinterpret_cast<void *>(&hookAlignedFree), true,
                          error) &&
            prepareExport(ucrt, "_malloc_base", real_malloc_base, reinterpret_cast<void *>(&hookMallocBase), false,
                          error) &&
            prepareExport(ucrt, "_calloc_base", real_calloc_base, reinterpret_cast<void *>(&hookCallocBase), false,
                          error) &&
            prepareExport(ucrt, "_realloc_base", real_realloc_base, reinterpret_cast<void *>(&hookReallocBase), false,
                          error) &&
            prepareExport(ucrt, "_free_base", real_free_base, reinterpret_cast<void *>(&hookFreeBase), false, error) &&
            prepareExport(kernel32, "HeapAlloc", real_heap_alloc, reinterpret_cast<void *>(&hookHeapAlloc), false,
                          error) &&
            prepareExport(kernel32, "HeapReAlloc", real_heap_realloc, reinterpret_cast<void *>(&hookHeapReAlloc), false,
                          error) &&
            prepareExport(kernel32, "HeapFree", real_heap_free, reinterpret_cast<void *>(&hookHeapFree), true, error);

        if (!ok) {
            funchook_destroy(hooks);
            hooks = nullptr;
            clearFunctionPointers();
            return false;
        }

        if (!rebuildProtectedCodeRanges(error)) {
            funchook_destroy(hooks);
            hooks = nullptr;
            clearFunctionPointers();
            return false;
        }

        Impl *expected = nullptr;
        if (!mActiveInstance.compare_exchange_strong(expected, this, std::memory_order_release,
                                                     std::memory_order_relaxed) &&
            expected != this) {
            error = "another native allocation sampler backend is already active";
            funchook_destroy(hooks);
            hooks = nullptr;
            clearFunctionPointers();
            return false;
        }

        hooks_prepared = true;
        return true;
    }

    bool installHooks(std::string &error)
    {
        if (hooks_installed.load(std::memory_order_acquire)) {
            return true;
        }
        if (hook_state_unknown) {
            error = "allocation hook state is unknown after an earlier lifecycle failure";
            return false;
        }

        const int code = funchook_install(hooks, 0);
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            error = hookError("funchook_install", code);
            return false;
        }
        hooks_installed.store(true, std::memory_order_release);
        return true;
    }

    static bool anyActiveHookCalls() noexcept
    {
        return std::ranges::any_of(mActiveHookCalls, [](const HookCounter &counter) {
            return counter.value.load(std::memory_order_acquire) != 0;
        });
    }

    bool uninstallHooks(std::string &error)
    {
        if (!hooks_installed.load(std::memory_order_acquire)) {
            return true;
        }

        // The IAT backend closes the pinned shim admission gate first, drains
        // callbacks that already entered Spark, clears the callback table, and
        // only then restores IAT slots that are still owned by us. Suspending
        // process threads here would deadlock that drain and is unnecessary for
        // ownership-safe IAT compare/exchange.
        const int code = funchook_uninstall(hooks, 0);
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            error = hookError("funchook_uninstall", code);
            return false;
        }
        hooks_installed.store(false, std::memory_order_release);
        hook_state_unknown = false;
        return true;
    }

    bool waitForQuiescence(std::string &error) const
    {
        const std::uint64_t deadline = monotonicMs() + 30000;
        while (true) {
            SuspendedProcessThreads suspended;
            if (!suspended.suspendStable(error)) {
                return false;
            }
            bool instruction_in_protected_code = false;
            std::uint32_t inspect_failure = 0;
            std::uint32_t inspect_thread = 0;
            const bool inspected = suspended.anyInstructionPointerInRanges(
                protected_code_ranges, instruction_in_protected_code, inspect_failure, inspect_thread);
            const bool active_calls = anyActiveHookCalls();
            std::string resume_error;
            const bool resumed = suspended.resume(resume_error);
            if (!inspected) {
                if (!resumed) {
                    error = resume_error;
                    return false;
                }
                if (monotonicMs() >= deadline) {
                    error = "timed out waiting for stable thread contexts; "
                            "GetThreadContext failed for thread " +
                            std::to_string(inspect_thread) + ": " + std::to_string(inspect_failure);
                    return false;
                }
                ::Sleep(1);
                continue;
            }
            if (!resumed) {
                error = resume_error;
                return false;
            }
            if (!instruction_in_protected_code && !active_calls) {
                return true;
            }
            if (monotonicMs() >= deadline) {
                error = "timed out waiting for allocation hook/trampoline calls to leave the plugin";
                return false;
            }
            ::Sleep(1);
        }
    }

    bool destroyHooks(std::string &error)
    {
        if (hooks == nullptr) {
            releaseThreadStateRegistry();
            return true;
        }
        if (hooks_installed.load(std::memory_order_acquire)) {
            error = "cannot destroy allocation hook trampolines while entry hooks are installed";
            return false;
        }

        const int code = funchook_destroy(hooks);
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            error = "funchook_destroy failed (code " + std::to_string(code) + ")";
            return false;
        }
        hooks = nullptr;
        hooks_prepared = false;
        hook_state_unknown = false;
        Impl *expected = this;
        mActiveInstance.compare_exchange_strong(expected, nullptr, std::memory_order_release,
                                                std::memory_order_relaxed);
        clearFunctionPointers();
        releaseThreadStateRegistry();
        return true;
    }

    void releaseThreadStateRegistry() noexcept
    {
        if (tls_index != TLS_OUT_OF_INDEXES) {
            ::TlsFree(tls_index);
            tls_index = TLS_OUT_OF_INDEXES;
        }
        for (ThreadSamplingState &state : thread_states) {
            if (state.thread_handle != nullptr) {
                ::CloseHandle(state.thread_handle);
                state.thread_handle = nullptr;
            }
            state.teb.store(nullptr, std::memory_order_relaxed);
            state.registry_state.store(0, std::memory_order_relaxed);
        }
    }

    void clearFunctionPointers() noexcept
    {
        real_malloc = nullptr;
        real_calloc = nullptr;
        real_realloc = nullptr;
        real_recalloc = nullptr;
        real_free = nullptr;
        real_aligned_malloc = nullptr;
        real_aligned_realloc = nullptr;
        real_aligned_recalloc = nullptr;
        real_aligned_offset_malloc = nullptr;
        real_aligned_offset_realloc = nullptr;
        real_aligned_offset_recalloc = nullptr;
        real_aligned_free = nullptr;
        real_malloc_base = nullptr;
        real_calloc_base = nullptr;
        real_realloc_base = nullptr;
        real_free_base = nullptr;
        real_heap_alloc = nullptr;
        real_heap_realloc = nullptr;
        real_heap_free = nullptr;
        prepared_targets.clear();
        protected_code_ranges.clear();
        hook_capabilities.clear();
    }

    FrameKey frameKeyForAddress(std::uint64_t raw_address, std::string &module_path)
    {
        MEMORY_BASIC_INFORMATION memory{};
        std::uintptr_t module_base = 0;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        if (::VirtualQuery(reinterpret_cast<void *>(static_cast<std::uintptr_t>(raw_address)), &memory,
                           sizeof(memory)) != 0) {
            module_base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
        }

        ModuleId module_id = kInvalidModule;
        auto cache = module_cache.find(module_base);
        if (cache != module_cache.end()) {
            module_id = cache->second;
            module_path = aggregation.modules().path(module_id);
        }
        else {
            char path[MAX_PATH]{};
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            const DWORD length = module_base != 0 ? ::GetModuleFileNameA(reinterpret_cast<HMODULE>(module_base), path,
                                                                         static_cast<DWORD>(sizeof(path)))
                                                  : 0;
            module_path = length > 0 ? std::string(path, length) : std::string("unknown");
            module_id =
                aggregation
                    .internFrame(module_path, module_base != 0 ? raw_address - module_base : raw_address, raw_address)
                    .module;
            if (module_cache.size() < KMaxModuleCacheEntries) {
                module_cache.emplace(module_base, module_id);
            }
        }

        return FrameKey{.module = module_id,
                        .rva = module_base != 0 ? raw_address - module_base : raw_address,
                        .raw_address = raw_address};
    }

    bool buildSample(void *const *frames, std::uint16_t depth, std::uint64_t tick_id, std::uint64_t thread_id,
                     std::uint64_t os_thread_id, std::int32_t window, std::uint64_t weight, Sample &sample)
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

        bool leading = true;
        for (std::size_t i = 0; i < depth; ++i) {
            const auto raw = reinterpret_cast<std::uint64_t>(frames[i]);
            if (raw == 0) {
                continue;
            }
            std::string path;
            FrameKey key = frameKeyForAddress(raw, path);
            if (leading && isLeadingAllocatorRuntime(path)) {
                continue;
            }
            leading = false;
            sample.frames.push_back(key);
        }

        if (sample.frames.empty()) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    bool buildSnapshotSample(void *const *frames, std::uint16_t depth, std::uint64_t tick_id, std::uint64_t thread_id,
                             std::uint64_t os_thread_id, std::int32_t window, std::uint64_t weight, Sample &sample)
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

        bool leading = true;
        for (std::size_t i = 0; i < depth; ++i) {
            const auto raw = reinterpret_cast<std::uint64_t>(frames[i]);
            if (raw == 0) {
                continue;
            }
            std::string path;
            FrameKey key = frameKeyForAddress(raw, path);
            if (leading && isLeadingAllocatorRuntime(path)) {
                continue;
            }
            leading = false;
            sample.frames.push_back(key);
        }
        return !sample.frames.empty();
    }

    void processEvent(AllocationEvent *event)
    {
        if (event->thread_observation) {
            aggregation.observeThread(event->thread_id, event->os_thread_id);
            return;
        }
        Sample sample;
        if (!buildSample(event->frames, event->depth, event->tick_id, event->thread_id, event->os_thread_id,
                         event->window, event->weight_bytes, sample)) {
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

        PSLIST_ENTRY list = ::InterlockedFlushSList(&ready_events);
        while (list != nullptr) {
            PSLIST_ENTRY next = list->Next;
            auto *event = CONTAINING_RECORD(list, AllocationEvent, entry);
            processEvent(event);
            ready_event_count.fetch_sub(1, std::memory_order_relaxed);
            recycleEvent(event);
            list = next;
        }
    }

    void aggregatorLoop()
    {
        TrackingSuppressionGuard suppress(*this);
        if (config.fail_aggregator_for_testing) {
            throw std::runtime_error("injected allocation aggregator failure");
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
                [this](std::size_t shard) { return ::TryAcquireSRWLockShared(&live_index_locks[shard]) != 0; },
                [this](std::size_t shard) { ::ReleaseSRWLockShared(&live_index_locks[shard]); },
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
        module_cache.clear();
        current_tick.store(0, std::memory_order_relaxed);
        hook_calls.store(0, std::memory_order_relaxed);
        successful_allocation_calls.store(0, std::memory_order_relaxed);
        sampling_points.store(0, std::memory_order_relaxed);
        filtered_samples.store(0, std::memory_order_relaxed);
        observed_bytes.store(0, std::memory_order_relaxed);
        dropped_samples.store(0, std::memory_order_relaxed);
        dropped_events.store(0, std::memory_order_relaxed);
        dropped_tick_events.store(0, std::memory_order_relaxed);
        enqueued_samples.store(0, std::memory_order_relaxed);
        ready_event_count.store(0, std::memory_order_relaxed);
        ready_event_high_water.store(0, std::memory_order_relaxed);
        tracking_hook_calls.store(0, std::memory_order_relaxed);
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
        retained_age_ms_total.store(0, std::memory_order_relaxed);
        retained_age_ms_max.store(0, std::memory_order_relaxed);
        aggregator_failure.fill('\0');
        aggregator_failed.store(false, std::memory_order_release);
    }

    bool waitForTrackingQuiescence(std::string &error) noexcept
    {
        for (int attempt = 0; attempt < 5000; ++attempt) {
            if (tracking_hook_calls.load(std::memory_order_acquire) == 0) {
                return true;
            }
            ::Sleep(1);
        }
        try {
            error = "timed out waiting for allocation lifecycle hooks to quiesce";
        }
        catch (...) {
            error.clear();
        }
        return false;
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
        if (hook_state_unknown) {
            error = "allocation hook state is unknown after an earlier lifecycle failure";
            return false;
        }
        if (new_config.session_seed == 0) {
            error = "the allocation session seed is not available";
            return false;
        }
        if (new_config.interval_bytes <= 0) {
            error = "allocation sampling interval must be greater than zero";
            return false;
        }

        resetSession();
        config = new_config;
        aggregation.reset(config, recovery_sink);
        if (!aggregation.configure(error)) {
            return false;
        }
        interval_bytes.store(static_cast<std::uint64_t>(new_config.interval_bytes), std::memory_order_relaxed);
        const std::uint64_t new_generation = generation.fetch_add(1, std::memory_order_relaxed) + 1;
        sampling_seed.store(new_generation ^ monotonicMs() ^ new_config.session_seed, std::memory_order_relaxed);

        if (!prepareHooks(error) || !allocateEventPool(error)) {
            return false;
        }

        if (!installHooks(error)) {
            freeEventPool();
            return false;
        }

        aggregator_running.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        tracking.store(true, std::memory_order_release);
        try {
            TrackingSuppressionGuard suppress(*this);
            aggregator_thread = std::thread([this]() {
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
            if (!waitForTrackingQuiescence(quiescence_error)) {
                error = std::move(quiescence_error);
                return false;
            }
            freeEventPool();
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
        if (!waitForTrackingQuiescence(error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregator_thread.joinable()) {
            aggregator_thread.join();
        }
        if (config.live_only && !aggregator_failed.load(std::memory_order_acquire)) {
            finalizeLiveProfile();
        }
        freeEventPool();
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
        if (!waitForTrackingQuiescence(error)) {
            return false;
        }
        aggregator_running.store(false, std::memory_order_release);
        if (aggregator_thread.joinable()) {
            aggregator_thread.join();
        }
        freeEventPool();

        // funchook_uninstall is the compatibility entry point for the native
        // IAT backend. It performs the bounded pinned-shim gate/drain and
        // ownership-safe detach, so a process-wide retry loop is neither
        // necessary nor safe.
        if (hooks_installed.load(std::memory_order_acquire) && !uninstallHooks(error)) {
            return false;
        }

        return destroyHooks(error);
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
        const std::int32_t window = profiling_window::windowNow();
        aggregation.recordTick(window, mspt_ms);
    }
};

std::atomic<AllocationSampler::Impl *> AllocationSampler::Impl::mActiveInstance{nullptr};
std::array<AllocationSampler::Impl::HookCounter, 64> AllocationSampler::Impl::mActiveHookCalls{};

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
        std::fprintf(stderr, "[spark] allocation sampler shutdown failed: %s\n", error.c_str());
    }
    // Unsafe hook state cannot survive plugin unload.
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
    return impl_->hook_calls.load(std::memory_order_relaxed);
}

std::uint64_t AllocationSampler::successfulAllocationCalls() const
{
    return impl_->successful_allocation_calls.load(std::memory_order_relaxed);
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
    return impl_->observed_bytes.load(std::memory_order_relaxed);
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
    return impl_->ready_event_high_water.load(std::memory_order_relaxed);
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
    return impl_->hooks_prepared ? 2 : 0;
}

std::uint64_t AllocationSampler::skippedModuleCount() const  // NOLINT(readability-convert-member-functions-to-static)
{
    return 0;
}

std::uint64_t AllocationSampler::failedModuleCount() const  // NOLINT(readability-convert-member-functions-to-static)
{
    return 0;
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
           impl_->aggregation.threadIdentityCacheDrops() != 0 || impl_->aggregation.dataIncomplete();
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
    return impl_->hooks_installed.load(std::memory_order_acquire);
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
    return impl_->prepared_targets.size();
}

}  // namespace spark
