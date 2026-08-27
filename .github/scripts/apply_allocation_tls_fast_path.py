from pathlib import Path

path = Path("src/native/alloc/allocation_sampler_linux.cpp")
text = path.read_text()

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

old = """        ByteSamplingState &state = thread.bytes;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        const auto current_tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
"""
new = """        ByteSamplingState &state = thread.bytes;
        const std::uint64_t current_generation = generation.load(std::memory_order_relaxed);
        const std::uint64_t interval = interval_bytes.load(std::memory_order_relaxed);
        const std::uint64_t current_tid = thread.owner_tid.load(std::memory_order_relaxed);
"""
if text.count(old) != 1:
    raise SystemExit(f"expected one recordAllocation TID match, got {text.count(old)}")
text = text.replace(old, new, 1)
path.write_text(text)
