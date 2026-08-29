#ifndef ENDSTONE_SPARK_PYTHON_ATTRIBUTION_H
#define ENDSTONE_SPARK_PYTHON_ATTRIBUTION_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "native/sampler/types.h"

namespace spark {

using PythonCodeId = std::uint64_t;
inline constexpr ModuleId kPythonFrameModule = 0xfffffffeu;
inline constexpr PythonCodeId kInvalidPythonCodeId = 0;

inline bool isPythonFrame(const FrameKey &key) noexcept
{
    return key.module == kPythonFrameModule;
}

inline FrameKey pythonFrameKey(PythonCodeId code_id) noexcept
{
    return FrameKey{.module = kPythonFrameModule, .rva = code_id, .raw_address = 0};
}

enum class PythonExecutionEvent : std::uint8_t {
    Start = 0,
    Resume = 1,
    Throw = 2,
    Return = 3,
    Yield = 4,
    Unwind = 5,
};

enum class PythonCodeCategory : std::uint8_t {
    Plugin = 0,
    Stdlib = 1,
    External = 2,
    Endstone = 3,
    Unknown = 4,
};

inline std::string_view pythonCodeCategoryName(PythonCodeCategory category) noexcept
{
    switch (category) {
    case PythonCodeCategory::Plugin:
        return "plugin";
    case PythonCodeCategory::Stdlib:
        return "stdlib";
    case PythonCodeCategory::External:
        return "external";
    case PythonCodeCategory::Endstone:
        return "endstone";
    case PythonCodeCategory::Unknown:
        return "unknown";
    }
    return "unknown";
}

struct PythonCodeMetadata {
    PythonCodeId code_id = kInvalidPythonCodeId;
    std::string filename;
    std::string module;
    std::string function_name;
    std::string qualname;
    std::int32_t first_line = -1;
    PythonCodeCategory category = PythonCodeCategory::Unknown;
    std::string plugin_source;
};

struct PythonAttributionDiagnostics {
    bool supported = false;
    bool monitoring_active = false;
    std::string backend = "none";
    std::string python_version;
    std::string unavailable_reason;
    std::uint64_t py_start = 0;
    std::uint64_t py_resume = 0;
    std::uint64_t py_throw = 0;
    std::uint64_t py_return = 0;
    std::uint64_t py_yield = 0;
    std::uint64_t py_unwind = 0;
    std::uint64_t registered_threads = 0;
    std::uint64_t max_depth = 0;
    std::uint64_t overflows = 0;
    std::uint64_t snapshot_attempts = 0;
    std::uint64_t snapshot_failures = 0;
    std::uint64_t attribution_samples = 0;
    std::uint64_t native_only_samples = 0;
    std::uint64_t boundary_misses = 0;
    std::uint64_t thread_mismatches = 0;
    std::uint64_t unknown_code_ids = 0;
    std::uint64_t code_objects = 0;
    std::uint64_t plugin_code = 0;
    std::uint64_t stdlib_code = 0;
    std::uint64_t external_code = 0;
    std::uint64_t endstone_code = 0;
    std::uint64_t unknown_code = 0;
    std::uint64_t code_cache_hits = 0;
    std::uint64_t code_cache_misses = 0;
    std::uint64_t monitoring_callbacks_failed = 0;
};

struct PythonAttributionExport {
    PythonAttributionDiagnostics diagnostics;
    std::vector<PythonCodeMetadata> codes;
};

class PythonStackProvider {
public:
    static constexpr std::size_t kMaxDepth = 256;

    struct Snapshot {
        std::array<PythonCodeId, kMaxDepth> codes{};  // root -> leaf
        std::size_t depth = 0;
    };

    virtual ~PythonStackProvider() = default;
    virtual bool snapshot(std::uint64_t native_tid, Snapshot &out) noexcept = 0;
    virtual void recordSample(bool attributed, bool boundary_miss) noexcept = 0;
    virtual void recordUnknownCodeId() noexcept = 0;
    virtual PythonAttributionExport exportState() const = 0;
};

// Fixed-capacity, per-native-thread Python execution state. Writers are the
// monitored Python threads themselves. The statistical sampler only performs
// atomic reads guarded by a seqlock and never takes the GIL or a mutex.
class PythonShadowStack {
public:
    static constexpr std::size_t kThreadCapacity = 256;
    static constexpr std::size_t kMaxDepth = PythonStackProvider::kMaxDepth;

    void resetSession() noexcept
    {
        session_epoch_.fetch_add(1, std::memory_order_acq_rel);
        registered_threads_.store(0, std::memory_order_relaxed);
        max_depth_.store(0, std::memory_order_relaxed);
        overflows_.store(0, std::memory_order_relaxed);
        snapshot_attempts_.store(0, std::memory_order_relaxed);
        snapshot_failures_.store(0, std::memory_order_relaxed);
        thread_mismatches_.store(0, std::memory_order_relaxed);
        unknown_code_ids_.store(0, std::memory_order_relaxed);
        for (ThreadSlot &slot : slots_) {
            slot.sequence.store(0, std::memory_order_relaxed);
            slot.depth.store(0, std::memory_order_relaxed);
            slot.hidden_depth.store(0, std::memory_order_relaxed);
            for (auto &code : slot.codes) {
                code.store(0, std::memory_order_relaxed);
            }
            slot.tid.store(0, std::memory_order_release);
        }
    }

    void onEvent(std::uint64_t native_tid, PythonExecutionEvent event, PythonCodeId code_id) noexcept
    {
        if (native_tid == 0 || code_id == kInvalidPythonCodeId) {
            unknown_code_ids_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ThreadSlot *slot = writerSlot(native_tid);
        if (slot == nullptr) {
            thread_mismatches_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        switch (event) {
        case PythonExecutionEvent::Start:
        case PythonExecutionEvent::Resume:
        case PythonExecutionEvent::Throw:
            push(*slot, code_id);
            break;
        case PythonExecutionEvent::Return:
        case PythonExecutionEvent::Yield:
        case PythonExecutionEvent::Unwind:
            pop(*slot, code_id);
            break;
        }
    }

    void bootstrap(std::uint64_t native_tid, const PythonCodeId *codes, std::size_t depth) noexcept
    {
        if (native_tid == 0) {
            return;
        }
        ThreadSlot *slot = findOrClaim(native_tid);
        if (slot == nullptr) {
            thread_mismatches_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        writeBegin(*slot);
        const std::size_t visible = std::min(depth, kMaxDepth);
        for (std::size_t i = 0; i < visible; ++i) {
            slot->codes[i].store(codes[i], std::memory_order_relaxed);
        }
        slot->depth.store(static_cast<std::uint32_t>(visible), std::memory_order_relaxed);
        slot->hidden_depth.store(static_cast<std::uint32_t>(depth - visible), std::memory_order_relaxed);
        if (depth > kMaxDepth) {
            overflows_.fetch_add(depth - kMaxDepth, std::memory_order_relaxed);
        }
        updateMaxDepth(depth);
        writeEnd(*slot);
    }

    bool snapshot(std::uint64_t native_tid, PythonStackProvider::Snapshot &out) noexcept
    {
        out.depth = 0;
        snapshot_attempts_.fetch_add(1, std::memory_order_relaxed);
        ThreadSlot *slot = find(native_tid);
        if (slot == nullptr) {
            return true;
        }

        for (int attempt = 0; attempt < 2; ++attempt) {
            const std::uint64_t before = slot->sequence.load(std::memory_order_acquire);
            if ((before & 1U) != 0) {
                continue;
            }
            const std::size_t depth = std::min<std::size_t>(slot->depth.load(std::memory_order_relaxed), kMaxDepth);
            for (std::size_t i = 0; i < depth; ++i) {
                out.codes[i] = slot->codes[i].load(std::memory_order_relaxed);
            }
            const std::uint64_t after = slot->sequence.load(std::memory_order_acquire);
            if (before == after && (after & 1U) == 0) {
                out.depth = depth;
                return true;
            }
        }
        snapshot_failures_.fetch_add(1, std::memory_order_relaxed);
        out.depth = 0;
        return false;
    }

    std::uint64_t registeredThreads() const noexcept { return registered_threads_.load(std::memory_order_relaxed); }
    std::uint64_t maxDepth() const noexcept { return max_depth_.load(std::memory_order_relaxed); }
    std::uint64_t overflows() const noexcept { return overflows_.load(std::memory_order_relaxed); }
    std::uint64_t snapshotAttempts() const noexcept { return snapshot_attempts_.load(std::memory_order_relaxed); }
    std::uint64_t snapshotFailures() const noexcept { return snapshot_failures_.load(std::memory_order_relaxed); }
    std::uint64_t threadMismatches() const noexcept { return thread_mismatches_.load(std::memory_order_relaxed); }
    std::uint64_t unknownCodeIds() const noexcept { return unknown_code_ids_.load(std::memory_order_relaxed); }

private:
    struct ThreadSlot {
        std::atomic<std::uint64_t> tid{0};
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::uint32_t> depth{0};
        std::atomic<std::uint32_t> hidden_depth{0};
        std::array<std::atomic<PythonCodeId>, kMaxDepth> codes{};
    };

    static std::size_t slotIndex(std::uint64_t tid) noexcept
    {
        tid ^= tid >> 33;
        tid *= 0xff51afd7ed558ccdULL;
        tid ^= tid >> 33;
        return static_cast<std::size_t>(tid) & (kThreadCapacity - 1);
    }

    ThreadSlot *find(std::uint64_t tid) noexcept
    {
        if (tid == 0) {
            return nullptr;
        }
        const std::size_t start = slotIndex(tid);
        for (std::size_t probe = 0; probe < kThreadCapacity; ++probe) {
            ThreadSlot &slot = slots_[(start + probe) & (kThreadCapacity - 1)];
            const std::uint64_t owner = slot.tid.load(std::memory_order_acquire);
            if (owner == tid) {
                return &slot;
            }
            if (owner == 0) {
                return nullptr;
            }
        }
        return nullptr;
    }

    ThreadSlot *findOrClaim(std::uint64_t tid) noexcept
    {
        const std::size_t start = slotIndex(tid);
        for (std::size_t probe = 0; probe < kThreadCapacity; ++probe) {
            ThreadSlot &slot = slots_[(start + probe) & (kThreadCapacity - 1)];
            std::uint64_t owner = slot.tid.load(std::memory_order_acquire);
            if (owner == tid) {
                return &slot;
            }
            if (owner == 0 && slot.tid.compare_exchange_strong(owner, tid, std::memory_order_acq_rel)) {
                registered_threads_.fetch_add(1, std::memory_order_relaxed);
                return &slot;
            }
        }
        return nullptr;
    }

    ThreadSlot *writerSlot(std::uint64_t tid) noexcept
    {
        struct Cache {
            PythonShadowStack *owner = nullptr;
            std::uint64_t epoch = 0;
            std::uint64_t tid = 0;
            ThreadSlot *slot = nullptr;
        };
        thread_local Cache cache;
        const std::uint64_t epoch = session_epoch_.load(std::memory_order_acquire);
        if (cache.owner == this && cache.epoch == epoch && cache.tid == tid && cache.slot != nullptr &&
            cache.slot->tid.load(std::memory_order_acquire) == tid) {
            return cache.slot;
        }
        cache.owner = this;
        cache.epoch = epoch;
        cache.tid = tid;
        cache.slot = findOrClaim(tid);
        return cache.slot;
    }

    static void writeBegin(ThreadSlot &slot) noexcept { slot.sequence.fetch_add(1, std::memory_order_acq_rel); }

    static void writeEnd(ThreadSlot &slot) noexcept { slot.sequence.fetch_add(1, std::memory_order_release); }

    void push(ThreadSlot &slot, PythonCodeId code_id) noexcept
    {
        writeBegin(slot);
        const std::uint32_t hidden = slot.hidden_depth.load(std::memory_order_relaxed);
        const std::uint32_t depth = slot.depth.load(std::memory_order_relaxed);
        if (hidden != 0 || depth >= kMaxDepth) {
            slot.hidden_depth.store(hidden + 1, std::memory_order_relaxed);
            overflows_.fetch_add(1, std::memory_order_relaxed);
            updateMaxDepth(static_cast<std::uint64_t>(depth) + hidden + 1);
            writeEnd(slot);
            return;
        }
        slot.codes[depth].store(code_id, std::memory_order_relaxed);
        slot.depth.store(depth + 1, std::memory_order_relaxed);
        updateMaxDepth(static_cast<std::uint64_t>(depth) + 1);
        writeEnd(slot);
    }

    void pop(ThreadSlot &slot, PythonCodeId code_id) noexcept
    {
        writeBegin(slot);
        const std::uint32_t hidden = slot.hidden_depth.load(std::memory_order_relaxed);
        if (hidden != 0) {
            slot.hidden_depth.store(hidden - 1, std::memory_order_relaxed);
            writeEnd(slot);
            return;
        }
        const std::uint32_t depth = slot.depth.load(std::memory_order_relaxed);
        if (depth == 0) {
            thread_mismatches_.fetch_add(1, std::memory_order_relaxed);
            writeEnd(slot);
            return;
        }
        if (slot.codes[depth - 1].load(std::memory_order_relaxed) == code_id) {
            slot.depth.store(depth - 1, std::memory_order_relaxed);
            writeEnd(slot);
            return;
        }
        for (std::uint32_t i = depth; i-- > 0;) {
            if (slot.codes[i].load(std::memory_order_relaxed) == code_id) {
                slot.depth.store(i, std::memory_order_relaxed);
                thread_mismatches_.fetch_add(1, std::memory_order_relaxed);
                writeEnd(slot);
                return;
            }
        }
        thread_mismatches_.fetch_add(1, std::memory_order_relaxed);
        writeEnd(slot);
    }

    void updateMaxDepth(std::uint64_t depth) noexcept
    {
        std::uint64_t current = max_depth_.load(std::memory_order_relaxed);
        while (depth > current && !max_depth_.compare_exchange_weak(current, depth, std::memory_order_relaxed,
                                                                    std::memory_order_relaxed)) {
        }
    }

    std::array<ThreadSlot, kThreadCapacity> slots_{};
    std::atomic<std::uint64_t> session_epoch_{1};
    std::atomic<std::uint64_t> registered_threads_{0};
    std::atomic<std::uint64_t> max_depth_{0};
    std::atomic<std::uint64_t> overflows_{0};
    std::atomic<std::uint64_t> snapshot_attempts_{0};
    std::atomic<std::uint64_t> snapshot_failures_{0};
    std::atomic<std::uint64_t> thread_mismatches_{0};
    std::atomic<std::uint64_t> unknown_code_ids_{0};
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PYTHON_ATTRIBUTION_H
