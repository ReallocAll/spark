#include "alloc/allocation_sampler.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "allocation_sampler_linux.cpp requires Linux x86-64"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cpptrace/cpptrace.hpp>
#include <moodycamel/concurrentqueue.h>

#include "alloc/byte_sampler.h"
#include "alloc/elf_import_hooks.h"

namespace spark {
namespace {

constexpr std::size_t kStackDepth = 48;
constexpr std::size_t kEventCapacity = 16384;
constexpr std::size_t kEventRingSize = kEventCapacity + 1;
constexpr std::size_t kFramesToSkip = 4;

std::uint64_t monotonicMs() noexcept
{
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000 +
           static_cast<std::uint64_t>(value.tv_nsec) / 1000000;
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
    using ReallocArrayFn = void *(*)(void *, std::size_t, std::size_t);
    using AlignedAllocFn = void *(*)(std::size_t, std::size_t);
    using PosixMemalignFn = int (*)(void **, std::size_t, std::size_t);

    struct AllocationEvent {
        std::uint64_t weight_bytes = 0;
        std::uint64_t tick_id = 0;
        std::int32_t window = 0;
        std::uint16_t depth = 0;
        cpptrace::frame_ptr frames[kStackDepth]{};
    };

    struct TickEvent {
        std::uint64_t tick_id = 0;
        double mspt_ms = 0.0;
    };

    struct EventRing {
        AllocationEvent *storage = nullptr;
        std::atomic<std::size_t> producer{0};
        std::atomic<std::size_t> consumer{0};

        bool allocate(std::string &error)
        {
            const std::size_t bytes = sizeof(AllocationEvent) * kEventRingSize;
            void *memory = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (memory == MAP_FAILED) {
                storage = nullptr;
                error = "mmap for Linux allocation sample buffer failed: " +
                        std::string(std::strerror(errno));
                return false;
            }
            storage = static_cast<AllocationEvent *>(memory);
            producer.store(0, std::memory_order_relaxed);
            consumer.store(0, std::memory_order_relaxed);
            return true;
        }

        void release() noexcept
        {
            if (storage != nullptr) {
                ::munmap(storage, sizeof(AllocationEvent) * kEventRingSize);
                storage = nullptr;
            }
            producer.store(0, std::memory_order_relaxed);
            consumer.store(0, std::memory_order_relaxed);
        }

        AllocationEvent *producerSlot() noexcept
        {
            const std::size_t current = producer.load(std::memory_order_relaxed);
            const std::size_t next = current + 1 == kEventRingSize ? 0 : current + 1;
            if (next == consumer.load(std::memory_order_acquire)) {
                return nullptr;
            }
            return &storage[current];
        }

        void publish() noexcept
        {
            const std::size_t current = producer.load(std::memory_order_relaxed);
            producer.store(current + 1 == kEventRingSize ? 0 : current + 1,
                           std::memory_order_release);
        }

        AllocationEvent *consumerSlot() noexcept
        {
            const std::size_t current = consumer.load(std::memory_order_relaxed);
            if (current == producer.load(std::memory_order_acquire)) {
                return nullptr;
            }
            return &storage[current];
        }

        void consume() noexcept
        {
            const std::size_t current = consumer.load(std::memory_order_relaxed);
            consumer.store(current + 1 == kEventRingSize ? 0 : current + 1,
                           std::memory_order_release);
        }
    };

    class HookCallGuard {
    public:
        HookCallGuard() noexcept
        {
            active_hook_calls.fetch_add(1, std::memory_order_acq_rel);
        }
        ~HookCallGuard()
        {
            active_hook_calls.fetch_sub(1, std::memory_order_release);
        }
    };

    class TrackingCallGuard {
    public:
        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl)
        {
            if (!impl_.tracking.load(std::memory_order_acquire)) {
                return;
            }
            impl_.tracking_calls.fetch_add(1, std::memory_order_acq_rel);
            if (impl_.tracking.load(std::memory_order_acquire)) {
                active_ = true;
            }
            else {
                impl_.tracking_calls.fetch_sub(1, std::memory_order_release);
            }
        }
        ~TrackingCallGuard()
        {
            if (active_) {
                impl_.tracking_calls.fetch_sub(1, std::memory_order_release);
            }
        }
        explicit operator bool() const noexcept { return active_; }

    private:
        Impl &impl_;
        bool active_ = false;
    };

    class RecursionGuard {
    public:
        RecursionGuard() noexcept : owner_(!inside_hook)
        {
            if (owner_) {
                inside_hook = true;
            }
        }
        ~RecursionGuard()
        {
            if (owner_) {
                inside_hook = false;
            }
        }
        bool owner() const noexcept { return owner_; }

    private:
        bool owner_;
    };

    class TrackingSuppressionGuard {
    public:
        TrackingSuppressionGuard() noexcept : previous_(tracking_suppressed)
        {
            tracking_suppressed = true;
        }
        ~TrackingSuppressionGuard() { tracking_suppressed = previous_; }

    private:
        bool previous_;
    };

    static std::atomic<Impl *> active_instance;
    static std::atomic<std::uint64_t> active_hook_calls;
    static thread_local bool inside_hook;
    static thread_local bool tracking_suppressed;
    static thread_local ByteSamplingState thread_sampling;

    ElfImportHooks hooks;
    void *libc_handle = nullptr;
    MallocFn real_malloc = nullptr;
    CallocFn real_calloc = nullptr;
    ReallocFn real_realloc = nullptr;
    ReallocArrayFn real_reallocarray = nullptr;
    AlignedAllocFn real_aligned_alloc = nullptr;
    PosixMemalignFn real_posix_memalign = nullptr;
    std::vector<AllocationHookCapability> hook_capabilities;

    std::mutex lifecycle_mutex;
    AllocationSamplerConfig config{};
    std::atomic<bool> tracking{false};
    std::atomic<bool> running{false};
    std::atomic<bool> aggregator_running{false};
    std::atomic<bool> aggregator_failed{false};
    std::array<char, 256> aggregator_failure{};
    std::atomic<std::uint64_t> tracking_calls{0};
    std::atomic<std::uint64_t> target_tid{0};
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> interval_bytes{kDefaultAllocationIntervalBytes};
    std::atomic<std::uint64_t> sampling_seed{0};
    std::atomic<std::uint64_t> started_ms{0};
    std::atomic<std::uint64_t> sample_count{0};
    std::atomic<std::uint64_t> sampled_bytes{0};
    std::atomic<std::uint64_t> observed_bytes{0};
    std::atomic<std::uint64_t> dropped_samples{0};

    EventRing events;
    std::thread aggregator_thread;
    moodycamel::ConcurrentQueue<TickEvent> ticks;
    CallTree tree;
    ModuleTable modules;
    std::map<std::int32_t, WindowTickStats> window_ticks;
    std::unordered_map<std::uint64_t, std::vector<Sample>> buckets;
    std::vector<std::uint8_t> tick_decisions;

    static Impl *activeOrAbort() noexcept
    {
        Impl *impl = active_instance.load(std::memory_order_acquire);
        if (impl == nullptr) {
            std::abort();
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

    bool shouldTrackCurrentThread() const noexcept
    {
        if (!tracking.load(std::memory_order_relaxed) ||
            static_cast<std::uint64_t>(::syscall(SYS_gettid)) !=
                target_tid.load(std::memory_order_relaxed)) {
            return false;
        }
        return !tracking_suppressed;
    }

    void *handleMalloc(std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_malloc(size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion;
        if (!tracking_guard || !recursion.owner()) {
            return real_malloc(size);
        }
        void *result = real_malloc(size);
        if (result != nullptr) {
            recordAllocation(static_cast<std::uint64_t>(size));
        }
        return result;
    }

    void *handleCalloc(std::size_t count, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_calloc(count, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion;
        if (!tracking_guard || !recursion.owner()) {
            return real_calloc(count, size);
        }
        void *result = real_calloc(count, size);
        std::uint64_t bytes = 0;
        if (result != nullptr && checkedMultiply(count, size, bytes)) {
            recordAllocation(bytes);
        }
        return result;
    }

    void *handleRealloc(void *pointer, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_realloc(pointer, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion;
        if (!tracking_guard || !recursion.owner()) {
            return real_realloc(pointer, size);
        }
        void *result = real_realloc(pointer, size);
        if (result != nullptr && size != 0) {
            recordAllocation(static_cast<std::uint64_t>(size));
        }
        return result;
    }

    void *handleReallocArray(void *pointer, std::size_t count, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_reallocarray(pointer, count, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion;
        if (!tracking_guard || !recursion.owner()) {
            return real_reallocarray(pointer, count, size);
        }
        void *result = real_reallocarray(pointer, count, size);
        std::uint64_t bytes = 0;
        if (result != nullptr && checkedMultiply(count, size, bytes)) {
            recordAllocation(bytes);
        }
        return result;
    }

    void *handleAlignedAlloc(std::size_t alignment, std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_aligned_alloc(alignment, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion;
        if (!tracking_guard || !recursion.owner()) {
            return real_aligned_alloc(alignment, size);
        }
        void *result = real_aligned_alloc(alignment, size);
        if (result != nullptr) {
            recordAllocation(static_cast<std::uint64_t>(size));
        }
        return result;
    }

    int handlePosixMemalign(void **result_pointer, std::size_t alignment,
                            std::size_t size) noexcept
    {
        if (!shouldTrackCurrentThread()) {
            return real_posix_memalign(result_pointer, alignment, size);
        }
        TrackingCallGuard tracking_guard(*this);
        RecursionGuard recursion;
        if (!tracking_guard || !recursion.owner()) {
            return real_posix_memalign(result_pointer, alignment, size);
        }
        const int result = real_posix_memalign(result_pointer, alignment, size);
        if (result == 0 && result_pointer != nullptr && *result_pointer != nullptr) {
            recordAllocation(static_cast<std::uint64_t>(size));
        }
        return result;
    }

    void recordAllocation(std::uint64_t requested_bytes) noexcept
    {
        if (requested_bytes == 0) {
            return;
        }
        observed_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
        ByteSamplingState &state = thread_sampling;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        if (state.generation != current_generation) {
            resetByteSamplingState(
                state, current_generation,
                sampling_seed.load(std::memory_order_relaxed) ^ current_generation ^
                    static_cast<std::uint64_t>(::syscall(SYS_gettid)),
                interval);
        }
        const std::uint64_t points = consumeSampledBytes(state, requested_bytes, interval);
        if (points == 0) {
            return;
        }

        AllocationEvent *event = events.producerSlot();
        if (event == nullptr) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        event->weight_bytes = saturatingMultiply(points, interval);
        event->tick_id = current_tick.load(std::memory_order_relaxed);
        const std::uint64_t now = monotonicMs();
        const std::uint64_t started = started_ms.load(std::memory_order_relaxed);
        event->window = static_cast<std::int32_t>(
            started != 0 && now >= started ? (now - started) / 1000 : 0);
        event->depth = static_cast<std::uint16_t>(cpptrace::safe_generate_raw_trace(
            event->frames, kStackDepth, kFramesToSkip));
        if (event->depth == 0) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        events.publish();
    }

    template <typename Function>
    bool resolveLibc(const char *name, Function &function, bool required,
                     std::string &error)
    {
        ::dlerror();
        function = reinterpret_cast<Function>(::dlsym(libc_handle, name));
        const char *failure = ::dlerror();
        if (function == nullptr && required) {
            error = std::string("required glibc allocator export not found: ") + name;
            if (failure != nullptr) {
                error += ": ";
                error += failure;
            }
            return false;
        }
        return true;
    }

    bool prepareHooks(std::string &error)
    {
        if (!hook_capabilities.empty()) {
            return true;
        }
        libc_handle = ::dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
        if (libc_handle == nullptr) {
            error = std::string("dlopen(libc.so.6) failed: ") + ::dlerror();
            return false;
        }
        if (!resolveLibc("malloc", real_malloc, true, error) ||
            !resolveLibc("calloc", real_calloc, true, error) ||
            !resolveLibc("realloc", real_realloc, true, error) ||
            !resolveLibc("reallocarray", real_reallocarray, false, error) ||
            !resolveLibc("aligned_alloc", real_aligned_alloc, false, error) ||
            !resolveLibc("posix_memalign", real_posix_memalign, false, error)) {
            ::dlclose(libc_handle);
            libc_handle = nullptr;
            return false;
        }

        const std::array specs{
            ElfImportHookSpec{"malloc", reinterpret_cast<void *>(&hookMalloc), true},
            ElfImportHookSpec{"calloc", reinterpret_cast<void *>(&hookCalloc), true},
            ElfImportHookSpec{"realloc", reinterpret_cast<void *>(&hookRealloc), true},
            ElfImportHookSpec{"reallocarray", reinterpret_cast<void *>(&hookReallocArray), false},
            ElfImportHookSpec{"aligned_alloc", reinterpret_cast<void *>(&hookAlignedAlloc), false},
            ElfImportHookSpec{"posix_memalign", reinterpret_cast<void *>(&hookPosixMemalign), false},
        };
        if (!hooks.prepare(specs, error)) {
            ::dlclose(libc_handle);
            libc_handle = nullptr;
            return false;
        }
        for (const ElfImportHookCapability &capability : hooks.capabilities()) {
            hook_capabilities.push_back(
                {capability.name,
                 capability.available ? AllocationHookStatus::Active
                                      : AllocationHookStatus::Missing,
                 capability.detail});
        }

        cpptrace::frame_ptr warm[8]{};
        cpptrace::safe_generate_raw_trace(warm, 8, 0);
        if (warm[0] != 0) {
            cpptrace::safe_object_frame object;
            cpptrace::get_safe_object_frame(warm[0], &object);
        }
        return true;
    }

    bool installHooks(std::string &error)
    {
        if (hooks.installed()) {
            return true;
        }
        Impl *expected = nullptr;
        if (!active_instance.compare_exchange_strong(expected, this,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed) && expected != this) {
            error = "another native allocation sampler backend is already active";
            return false;
        }
        if (!hooks.install(error)) {
            expected = this;
            active_instance.compare_exchange_strong(expected, nullptr,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    bool waitFor(std::atomic<std::uint64_t> &counter, const char *description,
                 std::string &error) noexcept
    {
        for (int attempt = 0; attempt < 5000; ++attempt) {
            if (counter.load(std::memory_order_acquire) == 0) {
                return true;
            }
            timespec delay{0, 1000000};
            ::nanosleep(&delay, nullptr);
        }
        try {
            error = std::string("timed out waiting for ") + description + " to quiesce";
        }
        catch (...) {
        }
        return false;
    }

    FrameKey frameKey(cpptrace::frame_ptr address)
    {
        cpptrace::safe_object_frame object;
        cpptrace::get_safe_object_frame(address, &object);
        std::string_view path = object.object_path[0] != '\0'
                                    ? std::string_view(object.object_path)
                                    : std::string_view("unknown");
        FrameKey key;
        key.module = modules.intern(path);
        key.rva = static_cast<std::uint64_t>(object.address_relative_to_object_start);
        key.raw_address = static_cast<std::uint64_t>(object.raw_address);
        return key;
    }

    void acceptSample(const Sample &sample)
    {
        tree.log(sample.frames, sample.window, sample.weight);
        sample_count.fetch_add(1, std::memory_order_relaxed);
        sampled_bytes.fetch_add(sample.weight, std::memory_order_relaxed);
    }

    void processEvent(const AllocationEvent &event)
    {
        Sample sample;
        sample.tick_id = event.tick_id;
        sample.window = event.window;
        sample.weight = event.weight_bytes;
        sample.frames.reserve(event.depth);
        for (std::size_t i = 0; i < event.depth; ++i) {
            if (event.frames[i] != 0) {
                sample.frames.push_back(frameKey(event.frames[i]));
            }
        }
        if (sample.frames.empty()) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (config.only_ticks_over_ms <= 0) {
            acceptSample(sample);
        }
        else if (sample.tick_id < tick_decisions.size() &&
                 tick_decisions[static_cast<std::size_t>(sample.tick_id)] != 0) {
            if (tick_decisions[static_cast<std::size_t>(sample.tick_id)] == 2) {
                acceptSample(sample);
            }
        }
        else {
            buckets[sample.tick_id].push_back(std::move(sample));
        }
    }

    void flushOrDrop(std::uint64_t tick_id, bool keep)
    {
        auto found = buckets.find(tick_id);
        if (found == buckets.end()) {
            return;
        }
        if (keep) {
            for (const Sample &sample : found->second) {
                acceptSample(sample);
            }
        }
        buckets.erase(found);
    }

    void drainQueues()
    {
        TickEvent tick;
        while (ticks.try_dequeue(tick)) {
            const bool keep = config.only_ticks_over_ms <= 0 ||
                              tick.mspt_ms > static_cast<double>(config.only_ticks_over_ms);
            if (config.only_ticks_over_ms > 0) {
                if (tick_decisions.size() <= tick.tick_id) {
                    tick_decisions.resize(static_cast<std::size_t>(tick.tick_id + 1), 0);
                }
                tick_decisions[static_cast<std::size_t>(tick.tick_id)] = keep ? 2 : 1;
            }
            flushOrDrop(tick.tick_id, keep);
        }
        while (AllocationEvent *event = events.consumerSlot()) {
            processEvent(*event);
            events.consume();
        }
    }

    void aggregatorLoop()
    {
        if (config.fail_aggregator_for_testing) {
            throw std::runtime_error("injected allocation aggregator failure");
        }
        while (aggregator_running.load(std::memory_order_acquire)) {
            drainQueues();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drainQueues();
        buckets.clear();
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
        while (ticks.try_dequeue(tick)) {
        }
        tree = CallTree{};
        modules = ModuleTable{};
        window_ticks.clear();
        buckets.clear();
        tick_decisions.clear();
        current_tick.store(0, std::memory_order_relaxed);
        sample_count.store(0, std::memory_order_relaxed);
        sampled_bytes.store(0, std::memory_order_relaxed);
        observed_bytes.store(0, std::memory_order_relaxed);
        dropped_samples.store(0, std::memory_order_relaxed);
        tracking_calls.store(0, std::memory_order_relaxed);
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
        if (new_config.target_tid == 0 || new_config.interval_bytes <= 0) {
            error = "invalid Linux allocation sampler configuration";
            return false;
        }
        if (new_config.live_only) {
            error = "retained allocation profiling is not yet enabled by the Linux backend";
            return false;
        }

        resetSession();
        config = new_config;
        target_tid.store(new_config.target_tid, std::memory_order_relaxed);
        interval_bytes.store(static_cast<std::uint64_t>(new_config.interval_bytes),
                             std::memory_order_relaxed);
        started_ms.store(monotonicMs(), std::memory_order_relaxed);
        const std::uint64_t next_generation =
            generation.fetch_add(1, std::memory_order_relaxed) + 1;
        sampling_seed.store(next_generation ^ monotonicMs() ^ new_config.target_tid,
                            std::memory_order_relaxed);

        if (!prepareHooks(error) || !events.allocate(error) || !installHooks(error)) {
            events.release();
            return false;
        }

        aggregator_running.store(true, std::memory_order_release);
        try {
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
            aggregator_running.store(false, std::memory_order_release);
            events.release();
            error = "could not create the allocation aggregator thread";
            return false;
        }
        running.store(true, std::memory_order_release);
        tracking.store(true, std::memory_order_release);
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
        events.release();
        if (aggregator_failed.load(std::memory_order_acquire)) {
            error = "allocation aggregator failed: " + std::string(aggregator_failure.data());
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

        if (hooks.installed() && !hooks.uninstall(error)) {
            return false;
        }
        if (!waitFor(active_hook_calls, "Linux allocation hook thunks", error)) {
            return false;
        }
        Impl *expected = this;
        active_instance.compare_exchange_strong(expected, nullptr,
                                                std::memory_order_release,
                                                std::memory_order_relaxed);
        if (libc_handle != nullptr) {
            ::dlclose(libc_handle);
            libc_handle = nullptr;
        }
        return true;
    }

    void tick(double mspt_ms)
    {
        if (!running.load(std::memory_order_acquire) ||
            aggregator_failed.load(std::memory_order_acquire)) {
            return;
        }
        TrackingSuppressionGuard suppress;
        const std::uint64_t finished = current_tick.fetch_add(1, std::memory_order_relaxed);
        ticks.enqueue(TickEvent{finished, mspt_ms});
        const std::uint64_t now = monotonicMs();
        const std::uint64_t started = started_ms.load(std::memory_order_relaxed);
        const std::int32_t window = static_cast<std::int32_t>(
            started != 0 && now >= started ? (now - started) / 1000 : 0);
        WindowTickStats &stats = window_ticks[window];
        ++stats.ticks;
        stats.mspt_sum += mspt_ms;
        stats.mspt_max = (std::max)(stats.mspt_max, mspt_ms);
    }
};

std::atomic<AllocationSampler::Impl *> AllocationSampler::Impl::active_instance{nullptr};
std::atomic<std::uint64_t> AllocationSampler::Impl::active_hook_calls{0};
thread_local bool AllocationSampler::Impl::inside_hook = false;
thread_local bool AllocationSampler::Impl::tracking_suppressed = false;
thread_local ByteSamplingState AllocationSampler::Impl::thread_sampling{};

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
        std::fprintf(stderr, "[spark] Linux allocation sampler shutdown failed: %s\n",
                     error.c_str());
    }
    std::abort();
}

bool AllocationSampler::start(const AllocationSamplerConfig &config, std::string &error)
{
    return impl_->startSession(config, error);
}

bool AllocationSampler::stop(std::string &error) { return impl_->stopSession(error); }
bool AllocationSampler::shutdown(std::string &error) { return impl_->shutdownBackend(error); }
void AllocationSampler::onTick(double mspt_ms) { impl_->tick(mspt_ms); }
const CallTree &AllocationSampler::tree() const { return impl_->tree; }
const ModuleTable &AllocationSampler::modules() const { return impl_->modules; }
const std::map<std::int32_t, WindowTickStats> &AllocationSampler::windowTicks() const
{
    return impl_->window_ticks;
}
std::uint64_t AllocationSampler::numberOfTicks() const
{
    return impl_->current_tick.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::sampleCount() const
{
    return impl_->sample_count.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::sampledBytes() const
{
    return impl_->sampled_bytes.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::observedBytes() const
{
    return impl_->observed_bytes.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::droppedSamples() const
{
    return impl_->dropped_samples.load(std::memory_order_relaxed);
}
std::uint64_t AllocationSampler::freedSamples() const { return 0; }
std::uint64_t AllocationSampler::freedBytes() const { return 0; }
std::uint64_t AllocationSampler::liveSamples() const { return 0; }
std::uint64_t AllocationSampler::liveBytes() const { return 0; }
std::uint64_t AllocationSampler::averageLifetimeMs() const { return 0; }
std::uint64_t AllocationSampler::maximumLifetimeMs() const { return 0; }
std::uint64_t AllocationSampler::lifecycleDropped() const { return 0; }
std::uint64_t AllocationSampler::retainedAverageAgeMs() const { return 0; }
std::uint64_t AllocationSampler::retainedMaximumAgeMs() const { return 0; }
bool AllocationSampler::running() const
{
    return impl_->running.load(std::memory_order_acquire);
}
bool AllocationSampler::hooksInstalled() const { return impl_->hooks.installed(); }
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
std::size_t AllocationSampler::hookTargetCount() const { return impl_->hooks.targetCount(); }

}  // namespace spark
