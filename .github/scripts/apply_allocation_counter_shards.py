from pathlib import Path

path = Path("src/native/alloc/allocation_sampler_linux.cpp")
text = path.read_text()


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, got {count}: {old[:160]!r}")
    text = text.replace(old, new, 1)


# Allocator hooks run concurrently on many BDS threads. The original global
# quiescence/tracking counters forced every malloc/free through the same cache
# line. Shard those counters by a stable per-thread TLS address while keeping
# the same shutdown rule: uninstall hooks first, then wait for every shard to
# reach zero before clearing the active backend.
replace_once(
    "constexpr std::size_t KLivePresenceBuckets = KLiveIndexCapacity;\n"
    "constexpr std::size_t KMaxSampledThreads = 256;",
    "constexpr std::size_t KLivePresenceBuckets = KLiveIndexCapacity;\n"
    "constexpr std::size_t KHookCallShards = 1024;\n"
    "constexpr std::size_t KMaxSampledThreads = 256;",
)

replace_once(
    """    inline static thread_local bool mCountOnlyInsideHook = false;

    class HookCallGuard {
    public:
        HookCallGuard() noexcept { mActiveHookCalls.fetch_add(1, std::memory_order_acq_rel); }
        ~HookCallGuard() { mActiveHookCalls.fetch_sub(1, std::memory_order_release); }
    };
""",
    """    inline static thread_local bool mCountOnlyInsideHook = false;
    inline static thread_local std::uint8_t mHookShardAnchor = 0;

    static std::size_t currentHookShard() noexcept
    {
        std::uintptr_t value = reinterpret_cast<std::uintptr_t>(&mHookShardAnchor);
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
""",
)

replace_once(
    """    class TrackingCallGuard {
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
""",
    """    class TrackingCallGuard {
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
""",
)

replace_once(
    "    static std::atomic<std::uint64_t> mActiveHookCalls;",
    "    static std::array<std::atomic<std::uint64_t>, KHookCallShards> mActiveHookCalls;",
)
replace_once(
    "    std::atomic<std::uint64_t> tracking_calls{0};",
    "    std::array<std::atomic<std::uint64_t>, KHookCallShards> tracking_calls{};",
)

replace_once(
    """    static bool waitFor(std::atomic<std::uint64_t> &counter, const char *description, std::string &error) noexcept
    {
        for (int attempt = 0; attempt < 5000; ++attempt) {
            if (counter.load(std::memory_order_acquire) == 0) {
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
""",
    """    template <std::size_t N>
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
""",
)

replace_once(
    "        tracking_calls.store(0, std::memory_order_relaxed);",
    """        for (auto &counter : tracking_calls) {
            counter.store(0, std::memory_order_relaxed);
        }""",
)

replace_once(
    "std::atomic<std::uint64_t> AllocationSampler::Impl::mActiveHookCalls{0};",
    "std::array<std::atomic<std::uint64_t>, KHookCallShards> AllocationSampler::Impl::mActiveHookCalls{};",
)

path.write_text(text)
