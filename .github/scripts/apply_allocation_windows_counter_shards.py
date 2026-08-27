from pathlib import Path

path = Path("src/native/alloc/allocation_sampler_windows.cpp")
text = path.read_text()


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one marker, found {count}: {old[:120]!r}")
    text = text.replace(old, new, 1)


replace_once(
    "constexpr std::size_t KLiveIndexShardCapacity = KLiveIndexCapacity / KLiveIndexShards;\n",
    "constexpr std::size_t KLiveIndexShardCapacity = KLiveIndexCapacity / KLiveIndexShards;\n"
    "constexpr std::size_t KWindowsHotCounterShards = 256;\n"
    "static_assert((KWindowsHotCounterShards & (KWindowsHotCounterShards - 1)) == 0);\n",
)

replace_once(
    "    struct alignas(64) HookCounter {\n"
    "        std::atomic<std::uint64_t> value{0};\n"
    "    };\n\n"
    "    static std::atomic<Impl *> mActiveInstance;\n",
    "    struct alignas(64) HookCounter {\n"
    "        std::atomic<std::uint64_t> value{0};\n"
    "    };\n\n"
    "    struct alignas(64) HotCounters {\n"
    "        std::atomic<std::uint64_t> hook_calls{0};\n"
    "        std::atomic<std::uint64_t> successful_allocation_calls{0};\n"
    "        std::atomic<std::uint64_t> observed_bytes{0};\n"
    "        std::atomic<std::uint64_t> tracking_hook_calls{0};\n"
    "    };\n"
    "    static_assert(sizeof(HotCounters) == 64);\n\n"
    "    static std::atomic<Impl *> mActiveInstance;\n",
)

replace_once(
    "    inline static thread_local bool mCountOnlyInsideHook = false;\n\n"
    "    class HookCallGuard {\n",
    "    inline static thread_local bool mCountOnlyInsideHook = false;\n\n"
    "    static std::size_t currentHotCounterShard() noexcept\n"
    "    {\n"
    "        std::uintptr_t value = reinterpret_cast<std::uintptr_t>(::NtCurrentTeb()) >> 12;\n"
    "        value ^= value >> 17;\n"
    "        value *= 0x9e3779b97f4a7c15ULL;\n"
    "        value ^= value >> 29;\n"
    "        return static_cast<std::size_t>(value & (KWindowsHotCounterShards - 1));\n"
    "    }\n\n"
    "    HotCounters &hotCountersForCurrentThread() noexcept { return hot_counters[currentHotCounterShard()]; }\n\n"
    "    class HookCallGuard {\n",
)

replace_once(
    "    class TrackingCallGuard {\n"
    "    public:\n"
    "        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl)\n"
    "        {\n"
    "            if (!impl_.tracking.load(std::memory_order_acquire)) {\n"
    "                return;\n"
    "            }\n"
    "            impl_.tracking_hook_calls.fetch_add(1, std::memory_order_acq_rel);\n"
    "            if (impl_.tracking.load(std::memory_order_acquire)) {\n"
    "                active_ = true;\n"
    "                return;\n"
    "            }\n"
    "            impl_.tracking_hook_calls.fetch_sub(1, std::memory_order_release);\n"
    "        }\n\n"
    "        ~TrackingCallGuard()\n"
    "        {\n"
    "            if (active_) {\n"
    "                impl_.tracking_hook_calls.fetch_sub(1, std::memory_order_release);\n"
    "            }\n"
    "        }\n\n"
    "        explicit operator bool() const noexcept { return active_; }\n\n"
    "    private:\n"
    "        Impl &impl_;\n"
    "        bool active_ = false;\n"
    "    };\n",
    "    class TrackingCallGuard {\n"
    "    public:\n"
    "        explicit TrackingCallGuard(Impl &impl) noexcept : impl_(impl), counters_(&impl.hotCountersForCurrentThread())\n"
    "        {\n"
    "            if (!impl_.tracking.load(std::memory_order_acquire)) {\n"
    "                return;\n"
    "            }\n"
    "            counters_->tracking_hook_calls.fetch_add(1, std::memory_order_acq_rel);\n"
    "            if (impl_.tracking.load(std::memory_order_acquire)) {\n"
    "                active_ = true;\n"
    "                return;\n"
    "            }\n"
    "            counters_->tracking_hook_calls.fetch_sub(1, std::memory_order_release);\n"
    "        }\n\n"
    "        ~TrackingCallGuard()\n"
    "        {\n"
    "            if (active_) {\n"
    "                counters_->tracking_hook_calls.fetch_sub(1, std::memory_order_release);\n"
    "            }\n"
    "        }\n\n"
    "        explicit operator bool() const noexcept { return active_; }\n\n"
    "    private:\n"
    "        Impl &impl_;\n"
    "        HotCounters *counters_ = nullptr;\n"
    "        bool active_ = false;\n"
    "    };\n",
)

replace_once(
    "    std::atomic<std::uint64_t> sampling_seed{0};\n"
    "    std::atomic<std::uint64_t> hook_calls{0};\n"
    "    std::atomic<std::uint64_t> successful_allocation_calls{0};\n"
    "    std::atomic<std::uint64_t> sampling_points{0};\n"
    "    std::atomic<std::uint64_t> filtered_samples{0};\n"
    "    std::atomic<std::uint64_t> observed_bytes{0};\n",
    "    std::atomic<std::uint64_t> sampling_seed{0};\n"
    "    std::array<HotCounters, KWindowsHotCounterShards> hot_counters{};\n"
    "    std::atomic<std::uint64_t> sampling_points{0};\n"
    "    std::atomic<std::uint64_t> filtered_samples{0};\n",
)

replace_once(
    "    std::atomic<std::uint64_t> ready_event_high_water{0};\n"
    "    std::atomic<std::uint64_t> tracking_hook_calls{0};\n"
    "    std::atomic<std::uint64_t> next_allocation_id{1};\n",
    "    std::atomic<std::uint64_t> ready_event_high_water{0};\n"
    "    std::atomic<std::uint64_t> next_allocation_id{1};\n",
)

replace_once(
    "        if (self->tracking.load(std::memory_order_relaxed)) {\n"
    "            self->hook_calls.fetch_add(1, std::memory_order_relaxed);\n"
    "        }\n",
    "        if (self->tracking.load(std::memory_order_relaxed)) {\n"
    "            self->hotCountersForCurrentThread().hook_calls.fetch_add(1, std::memory_order_relaxed);\n"
    "        }\n",
)

replace_once(
    "    void recordAllocation(void *pointer, std::uint64_t requested_bytes) noexcept\n"
    "    {\n"
    "        successful_allocation_calls.fetch_add(1, std::memory_order_relaxed);\n"
    "        if (requested_bytes == 0) {\n"
    "            return;\n"
    "        }\n"
    "        observed_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);\n",
    "    void recordAllocation(void *pointer, std::uint64_t requested_bytes) noexcept\n"
    "    {\n"
    "        HotCounters &counters = hotCountersForCurrentThread();\n"
    "        counters.successful_allocation_calls.fetch_add(1, std::memory_order_relaxed);\n"
    "        if (requested_bytes == 0) {\n"
    "            return;\n"
    "        }\n"
    "        counters.observed_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);\n",
)

replace_once(
    "        current_tick.store(0, std::memory_order_relaxed);\n"
    "        hook_calls.store(0, std::memory_order_relaxed);\n"
    "        successful_allocation_calls.store(0, std::memory_order_relaxed);\n"
    "        sampling_points.store(0, std::memory_order_relaxed);\n"
    "        filtered_samples.store(0, std::memory_order_relaxed);\n"
    "        observed_bytes.store(0, std::memory_order_relaxed);\n",
    "        current_tick.store(0, std::memory_order_relaxed);\n"
    "        for (HotCounters &counters : hot_counters) {\n"
    "            counters.hook_calls.store(0, std::memory_order_relaxed);\n"
    "            counters.successful_allocation_calls.store(0, std::memory_order_relaxed);\n"
    "            counters.observed_bytes.store(0, std::memory_order_relaxed);\n"
    "            counters.tracking_hook_calls.store(0, std::memory_order_relaxed);\n"
    "        }\n"
    "        sampling_points.store(0, std::memory_order_relaxed);\n"
    "        filtered_samples.store(0, std::memory_order_relaxed);\n",
)

replace_once(
    "        ready_event_count.store(0, std::memory_order_relaxed);\n"
    "        ready_event_high_water.store(0, std::memory_order_relaxed);\n"
    "        tracking_hook_calls.store(0, std::memory_order_relaxed);\n"
    "        next_allocation_id.store(1, std::memory_order_relaxed);\n",
    "        ready_event_count.store(0, std::memory_order_relaxed);\n"
    "        ready_event_high_water.store(0, std::memory_order_relaxed);\n"
    "        next_allocation_id.store(1, std::memory_order_relaxed);\n",
)

replace_once(
    "    bool waitForTrackingQuiescence(std::string &error) noexcept\n"
    "    {\n"
    "        for (int attempt = 0; attempt < 5000; ++attempt) {\n"
    "            if (tracking_hook_calls.load(std::memory_order_acquire) == 0) {\n"
    "                return true;\n"
    "            }\n"
    "            ::Sleep(1);\n"
    "        }\n",
    "    bool waitForTrackingQuiescence(std::string &error) noexcept\n"
    "    {\n"
    "        for (int attempt = 0; attempt < 5000; ++attempt) {\n"
    "            const bool active = std::ranges::any_of(hot_counters, [](const HotCounters &counters) {\n"
    "                return counters.tracking_hook_calls.load(std::memory_order_acquire) != 0;\n"
    "            });\n"
    "            if (!active) {\n"
    "                return true;\n"
    "            }\n"
    "            ::Sleep(1);\n"
    "        }\n",
)

replace_once(
    "std::uint64_t AllocationSampler::hookCalls() const\n"
    "{\n"
    "    return impl_->hook_calls.load(std::memory_order_relaxed);\n"
    "}\n\n"
    "std::uint64_t AllocationSampler::successfulAllocationCalls() const\n"
    "{\n"
    "    return impl_->successful_allocation_calls.load(std::memory_order_relaxed);\n"
    "}\n",
    "std::uint64_t AllocationSampler::hookCalls() const\n"
    "{\n"
    "    std::uint64_t total = 0;\n"
    "    for (const auto &counters : impl_->hot_counters) {\n"
    "        total += counters.hook_calls.load(std::memory_order_relaxed);\n"
    "    }\n"
    "    return total;\n"
    "}\n\n"
    "std::uint64_t AllocationSampler::successfulAllocationCalls() const\n"
    "{\n"
    "    std::uint64_t total = 0;\n"
    "    for (const auto &counters : impl_->hot_counters) {\n"
    "        total += counters.successful_allocation_calls.load(std::memory_order_relaxed);\n"
    "    }\n"
    "    return total;\n"
    "}\n",
)

replace_once(
    "std::uint64_t AllocationSampler::observedBytes() const\n"
    "{\n"
    "    return impl_->observed_bytes.load(std::memory_order_relaxed);\n"
    "}\n",
    "std::uint64_t AllocationSampler::observedBytes() const\n"
    "{\n"
    "    std::uint64_t total = 0;\n"
    "    for (const auto &counters : impl_->hot_counters) {\n"
    "        total += counters.observed_bytes.load(std::memory_order_relaxed);\n"
    "    }\n"
    "    return total;\n"
    "}\n",
)

path.write_text(text)
