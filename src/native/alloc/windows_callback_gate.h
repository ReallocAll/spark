#ifndef ENDSTONE_SPARK_WINDOWS_CALLBACK_GATE_H
#define ENDSTONE_SPARK_WINDOWS_CALLBACK_GATE_H

#include <atomic>
#include <cstdint>
#include <limits>

namespace spark {

// Tracks callbacks that have entered a stable, process-lifetime trampoline.
//
// This gate deliberately does NOT make an unloadable replacement function safe
// by itself: a thread can fetch an IAT replacement address before uninstall and
// call it later. The eventual Windows allocation backend must therefore place
// this gate in code/storage whose lifetime outlives the unloadable Spark plugin
// (for example a pinned shim), then close and drain the gate before publishing
// plugin-owned state as unavailable.
class WindowsCallbackLifetimeGate {
public:
    WindowsCallbackLifetimeGate() noexcept = default;

    WindowsCallbackLifetimeGate(const WindowsCallbackLifetimeGate &) = delete;
    WindowsCallbackLifetimeGate &operator=(const WindowsCallbackLifetimeGate &) = delete;

    [[nodiscard]] bool open() noexcept
    {
        std::uint64_t current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (!isClosed(current) || count(current) != 0 || epoch(current) == KMaxEpoch) {
                return false;
            }
            const std::uint64_t desired = current & ~KClosedBit;
            if (state_.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] bool tryEnter() noexcept
    {
        std::uint64_t current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (isClosed(current) || count(current) == KCountMask) {
                return false;
            }
            const std::uint64_t desired = current + 1;
            if (state_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] bool leave() noexcept
    {
        std::uint64_t current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (count(current) == 0) {
                return false;
            }
            const std::uint64_t desired = current - 1;
            if (state_.compare_exchange_weak(current, desired, std::memory_order_release, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    // Closes admission and advances the epoch atomically. Advancing the epoch
    // prevents a callback that read an old open state before close() from
    // succeeding with a delayed compare-exchange after a later reopen.
    //
    // If the epoch space is exhausted, the gate is still closed but returns
    // false and can never be reopened: fail closed rather than permit ABA.
    [[nodiscard]] bool close() noexcept
    {
        std::uint64_t current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (isClosed(current)) {
                return epoch(current) != KMaxEpoch;
            }

            const std::uint64_t current_epoch = epoch(current);
            const bool exhausted = current_epoch == KMaxEpoch;
            const std::uint64_t next_epoch = exhausted ? current_epoch : current_epoch + 1;
            const std::uint64_t desired = compose(next_epoch, true, count(current));
            if (state_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return !exhausted;
            }
        }
    }

    [[nodiscard]] bool closed() const noexcept { return isClosed(state_.load(std::memory_order_acquire)); }

    [[nodiscard]] bool drained() const noexcept
    {
        const std::uint64_t current = state_.load(std::memory_order_acquire);
        return isClosed(current) && count(current) == 0;
    }

    [[nodiscard]] std::uint32_t activeCount() const noexcept { return count(state_.load(std::memory_order_acquire)); }

    [[nodiscard]] std::uint32_t generation() const noexcept
    {
        return static_cast<std::uint32_t>(epoch(state_.load(std::memory_order_acquire)));
    }

private:
    static constexpr std::uint64_t KCountMask = 0x00000000FFFFFFFFULL;
    static constexpr std::uint64_t KClosedBit = 0x0000000100000000ULL;
    static constexpr unsigned KEpochShift = 33;
    static constexpr std::uint64_t KMaxEpoch = 0x7FFFFFFFULL;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "Windows allocation callback gate requires lock-free 64-bit atomics");

    [[nodiscard]] static constexpr std::uint32_t count(std::uint64_t state) noexcept
    {
        return static_cast<std::uint32_t>(state & KCountMask);
    }

    [[nodiscard]] static constexpr bool isClosed(std::uint64_t state) noexcept { return (state & KClosedBit) != 0; }

    [[nodiscard]] static constexpr std::uint64_t epoch(std::uint64_t state) noexcept { return state >> KEpochShift; }

    [[nodiscard]] static constexpr std::uint64_t compose(std::uint64_t generation, bool closed,
                                                         std::uint32_t active) noexcept
    {
        return (generation << KEpochShift) | (closed ? KClosedBit : 0) | active;
    }

    std::atomic<std::uint64_t> state_{KClosedBit};
};

}  // namespace spark

#endif
