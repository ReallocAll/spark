#include "native/sampler/sampler.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string_view>
#include <utility>

#include <cpptrace/cpptrace.hpp>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on
#endif

#include "native/sampler/capture.h"
#include "native/sampler/thread_info.h"
#include "native/symbol/symbolicate.h"

namespace spark {

namespace {
// Linux drops the signal handler and return trampoline; Windows drops nothing.
#ifdef _WIN32
constexpr std::size_t KLeadingDrop = 0;
#else
constexpr std::size_t KLeadingDrop = 2;
#endif

char asciiLower(char ch) noexcept
{
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool startsWithInsensitive(std::string_view value, std::string_view prefix) noexcept
{
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (asciiLower(value[i]) != asciiLower(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool isPythonRuntimeModule(std::string_view path) noexcept
{
    const std::size_t separator = path.find_last_of("/\\");
    const std::string_view name = separator == std::string_view::npos ? path : path.substr(separator + 1);
    return startsWithInsensitive(name, "libpython3") || startsWithInsensitive(name, "python3");
}

std::size_t pythonInsertionPoint(const std::vector<FrameKey> &frames, const ModuleTable &modules) noexcept
{
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].module == kInvalidModule || frames[i].module >= modules.size()) {
            continue;
        }
        if (isPythonRuntimeModule(modules.path(frames[i].module))) {
            return i;
        }
    }
    return frames.size();
}

}  // namespace

void Sampler::markWorkerFailure() noexcept
{
    worker_failed_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    agg_running_.store(false, std::memory_order_release);
    wait_cv_.notify_all();
}

bool Sampler::failure(std::string &error) const
{
    if (!worker_failed_.load(std::memory_order_acquire)) {
        error.clear();
        return false;
    }
    error = "the sampler service worker failed";
    return true;
}

bool Sampler::startServiceThreads()
{
    try {
        running_.store(true);
        agg_running_.store(true);
        aggregator_thread_ = std::thread([this] {
            try {
                if (aggregator_thread_hook_) {
                    aggregator_thread_hook_();
                }
                aggregatorLoop();
            }
            catch (...) {
                markWorkerFailure();
            }
        });
        sampler_thread_ = std::thread([this] {
            try {
                if (sampler_thread_hook_) {
                    sampler_thread_hook_();
                }
                samplerLoop();
            }
            catch (...) {
                markWorkerFailure();
            }
        });
        service_start_count_.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...) {
        running_.store(false);
        wait_cv_.notify_all();
        if (sampler_thread_.joinable()) {
            sampler_thread_.join();
        }
        agg_running_.store(false);
        if (aggregator_thread_.joinable()) {
            aggregator_thread_.join();
        }
        Capture::disarm();
        last_error_ = "the sampler service threads could not be started";
        return false;
    }
    return true;
}

Sampler::~Sampler()
{
    if (!stop()) {
        std::terminate();
    }
}

bool Sampler::start(const SamplerConfig &config)
{
    last_error_.clear();
    if (running_.load()) {
        last_error_ = "sampler is already running";
        return false;
    }
    config_ = config;
    if (!thread_selector_.configure(config_.all_threads, config_.regex_threads, config_.thread_patterns, last_error_)) {
        return false;
    }
    if (!Capture::arm()) {
        last_error_ = "the platform stack-capture backend could not be initialized";
        return false;
    }
    resetSession();
    if (!startServiceThreads()) {
        return false;
    }
    return true;
}

bool Sampler::stop()
{
    running_.store(false);
#ifdef _WIN32
    Capture::cancelPending();
#endif
    wait_cv_.notify_all();
    if (sampler_thread_.joinable()) {
        sampler_thread_.join();  // no more samples are produced after this
    }
    agg_running_.store(false);
    if (aggregator_thread_.joinable()) {
        aggregator_thread_.join();  // drains everything the sampler left behind
    }
    if (!Capture::disarm()) {
        last_error_ = "the stack-capture handler is still active";
        return false;
    }
    return true;
}

void Sampler::requestStop() noexcept
{
    running_.store(false, std::memory_order_release);
#ifdef _WIN32
    Capture::cancelPending();
#endif
    wait_cv_.notify_all();
}

void Sampler::pauseForExport()
{
    running_.store(false);
#ifdef _WIN32
    Capture::cancelPending();
#endif
    wait_cv_.notify_all();
    if (sampler_thread_.joinable()) {
        sampler_thread_.join();
    }
    agg_running_.store(false);
    if (aggregator_thread_.joinable()) {
        aggregator_thread_.join();
    }
    // Keep capture armed and session data intact for resume.
}

bool Sampler::resumeAfterExport()
{
    if (running_.load()) {
        return true;
    }
    if (!startServiceThreads()) {
        last_error_ = "the sampler service threads could not be resumed";
        return false;
    }
    return true;
}

void Sampler::resetSession()
{
    Sample sample;
    while (samples_.try_dequeue(sample)) {
    }
    TickEvent tick;
    while (ticks_.try_dequeue(tick)) {
    }

    tree_ = CallTree{};
    thread_trees_.clear();
    buckets_.clear();
    tick_decisions_.clear();
    tick_decision_base_ = 0;
    window_sample_counts_.clear();
    const std::int32_t current_window = currentWindow();
    next_history_prune_window_ = current_window;
    modules_ = ModuleTable{kModuleCapacity};
    pending_recovery_module_definitions_.clear();
    window_ticks_.clear();
    next_tick_history_prune_window_ = current_window;
    current_tick_.store(0);
    sample_count_.store(0, std::memory_order_relaxed);
    dropped_samples_.store(0, std::memory_order_relaxed);
    dropped_queue_samples_.store(0, std::memory_order_relaxed);
    dropped_pending_samples_.store(0, std::memory_order_relaxed);
    dropped_profile_samples_.store(0, std::memory_order_relaxed);
    dropped_tick_events_.store(0, std::memory_order_relaxed);
    module_overflow_frames_.store(0, std::memory_order_relaxed);
    overflow_thread_samples_.store(0, std::memory_order_relaxed);
    history_samples_pruned_.store(0, std::memory_order_relaxed);
    profile_storage_exhausted_.store(false, std::memory_order_relaxed);
    sampler_tid_.store(0, std::memory_order_relaxed);
    aggregator_tid_.store(0, std::memory_order_relaxed);
    worker_failed_.store(false, std::memory_order_relaxed);
    sampler_heartbeat_.sequence.store(0, std::memory_order_relaxed);
    sampler_heartbeat_.last_ns.store(0, std::memory_order_relaxed);
    aggregator_heartbeat_.sequence.store(0, std::memory_order_relaxed);
    aggregator_heartbeat_.last_ns.store(0, std::memory_order_relaxed);
    pending_sample_count_ = 0;
    profile_nodes_remaining_ = kProfileNodeCapacity;
    profile_time_entries_remaining_ = kProfileTimeEntryCapacity;
    thread_identities_.clear();
    journaled_threads_.clear();
}

std::int32_t Sampler::currentWindow()
{
    return profiling_window::windowNow();
}

void Sampler::onTick(double mspt_ms)
{
    std::uint64_t finished = current_tick_.load();
    if (!ticks_.try_enqueue(tick_producer_, TickEvent{.tick_id = finished, .mspt_ms = mspt_ms})) {
        dropped_tick_events_.fetch_add(1, std::memory_order_relaxed);
    }
    current_tick_.store(finished + 1);

    const std::int32_t window = currentWindow();
    WindowTickStats &w = window_ticks_[window];
    w.ticks += 1;
    w.mspt_sum += mspt_ms;
    w.mspt_max = std::max(mspt_ms, w.mspt_max);
    maybePruneTickHistory(window);
}

bool Sampler::enqueueSample(Sample sample) noexcept
{
    if (samples_.try_enqueue(sample_producer_, std::move(sample))) {
        return true;
    }
    dropped_queue_samples_.fetch_add(1, std::memory_order_relaxed);
    dropped_samples_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Sampler::samplerLoop()
{
    struct ThreadTiming {
        std::chrono::steady_clock::time_point last_attempt;
        std::uint64_t previous_capture_us = 0;
    };

    CaptureBuffer buf;
    const auto interval = std::chrono::microseconds(config_.interval_us);
    sampler_tid_.store(currentNativeThreadId(), std::memory_order_release);
    while (running_.load() && aggregator_tid_.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    std::vector<ThreadInfo> targets;
    std::unordered_map<std::uint64_t, ThreadTiming> timings;
    std::size_t next_target = 0;
    auto next_refresh = std::chrono::steady_clock::time_point{};
    while (running_.load()) {
        {
            std::unique_lock lock(wait_mutex_);
            if (wait_cv_.wait_for(lock, interval, [this] { return !running_.load(); })) {
                break;
            }
        }

        if (config_.all_threads || !config_.thread_patterns.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_refresh) {
                targets = enumerateProcessThreads();
                const std::uint64_t sampler_tid = sampler_tid_.load(std::memory_order_acquire);
                const std::uint64_t aggregator_tid = aggregator_tid_.load(std::memory_order_acquire);
                auto removed = std::ranges::remove_if(targets, [&](const ThreadInfo &thread) {
                    return thread.id == sampler_tid || thread.id == aggregator_tid;
                });
                targets.erase(removed.begin(), removed.end());
                if (!config_.all_threads) {
                    removed = std::ranges::remove_if(
                        targets, [&](const ThreadInfo &thread) { return !thread_selector_.matches(thread.name); });
                    targets.erase(removed.begin(), removed.end());
                }
                std::erase_if(timings, [&](const auto &entry) {
                    return std::none_of(targets.begin(), targets.end(),
                                        [&](const ThreadInfo &thread) { return thread.id == entry.first; });
                });
                for (ThreadInfo &thread : targets) {
                    thread.name += " (#" + std::to_string(thread.id) + ")";
                }
                next_refresh = now + std::chrono::seconds(1);
            }
        }
        else {
            const std::uint64_t tid = target_tid_.load();
            targets = tid == 0 ? std::vector<ThreadInfo>{} : std::vector<ThreadInfo>{{.id = tid, .name = target_name_}};
        }

        const std::size_t target_count = targets.size();
        for (std::size_t checked = 0; checked < target_count; ++checked) {
            if (!running_.load()) {
                break;
            }
            const ThreadInfo &target = targets[next_target % target_count];
            ++next_target;

            const bool target_running = !config_.ignore_sleeping || Capture::isThreadRunning(target.id);
            const auto attempt_time = std::chrono::steady_clock::now();
            auto [timing_it, inserted] = timings.try_emplace(target.id);
            ThreadTiming &timing = timing_it->second;
            auto elapsed_us = static_cast<std::uint64_t>(config_.interval_us);
            if (!inserted) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::microseconds>(attempt_time - timing.last_attempt);
                const std::uint64_t wall_us = elapsed.count() > 0 ? static_cast<std::uint64_t>(elapsed.count()) : 1;
                elapsed_us = wall_us > timing.previous_capture_us ? wall_us - timing.previous_capture_us : 1;
            }
            timing.last_attempt = attempt_time;
            timing.previous_capture_us = 0;

            if (!target_running) {
                continue;
            }
            const bool captured = Capture::captureThread(target.id, buf);
            const auto capture_elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - attempt_time);
            if (capture_elapsed.count() > 0) {
                timing.previous_capture_us = static_cast<std::uint64_t>(capture_elapsed.count());
            }
            if (!captured) {
                break;  // one potentially expensive stack-walk attempt per interval
            }

            PythonStackProvider *python_provider = python_stack_provider_.load(std::memory_order_acquire);
            PythonStackProvider::Snapshot python_snapshot;
            const bool python_snapshot_consistent =
                python_provider == nullptr || python_provider->snapshot(target.id, python_snapshot);

            Sample sample;
            sample.thread_id = target.id;
            sample.thread_name = target.name;
            sample.tick_id = current_tick_.load();
            sample.window = currentWindow();
            sample.weight = elapsed_us;
            sample.frames.reserve(buf.count + (python_snapshot_consistent ? python_snapshot.depth : 0));
            for (std::size_t i = KLeadingDrop; i < buf.count; ++i) {
#ifdef _WIN32
                auto raw_address = static_cast<std::uint64_t>(buf.ips[i]);
                DWORD64 module_base = SymGetModuleBase64(GetCurrentProcess(), raw_address);

                std::string path = "unknown";
                if (module_base != 0) {
                    char module_path[MAX_PATH]{};
                    DWORD length =
                        // NOLINTNEXTLINE(performance-no-int-to-ptr)
                        GetModuleFileNameA(reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(module_base)),
                                           module_path, static_cast<DWORD>(sizeof(module_path)));
                    if (length > 0) {
                        path.assign(module_path, length);
                    }
                }

                FrameKey key;
                const std::size_t prev_module_count = modules_.size();
                key.module = modules_.intern(path);
                if (key.module == 0 && path != kOtherModulesSentinel && modules_.size() == kModuleCapacity) {
                    module_overflow_frames_.fetch_add(1, std::memory_order_relaxed);
                }
                if (recovery_sink_ && modules_.size() > prev_module_count) {
                    pending_recovery_module_definitions_.push_back(
                        RecoveryModuleDefinition{.module_id = key.module, .path = std::string(path)});
                }
                key.rva = module_base != 0 ? raw_address - module_base : raw_address;
                key.raw_address = raw_address;
#else
                cpptrace::safe_object_frame frame;
                cpptrace::get_safe_object_frame(buf.ips[i], &frame);
                std::string_view path =
                    frame.object_path[0] != '\0' ? std::string_view(frame.object_path) : std::string_view("unknown");
                FrameKey key;
                const std::size_t prev_module_count = modules_.size();
                key.module = modules_.intern(path);
                if (key.module == 0 && path != kOtherModulesSentinel && modules_.size() == kModuleCapacity) {
                    module_overflow_frames_.fetch_add(1, std::memory_order_relaxed);
                }
                if (recovery_sink_ && modules_.size() > prev_module_count) {
                    pending_recovery_module_definitions_.push_back(
                        RecoveryModuleDefinition{.module_id = key.module, .path = std::string(path)});
                }
                key.rva = static_cast<std::uint64_t>(frame.address_relative_to_object_start);
                key.raw_address = static_cast<std::uint64_t>(frame.raw_address);
#endif
                sample.frames.push_back(key);
            }
            if (sample.frames.empty()) {
                break;
            }
            // Exclude wait states when sleeping threads are ignored.
            if (config_.ignore_sleeping && isSleepFrame(sample.frames.front().raw_address)) {
                break;
            }

            bool python_attributed = false;
            bool python_boundary_miss = false;
            if (python_provider != nullptr && python_snapshot_consistent && python_snapshot.depth != 0) {
                const std::size_t insertion = pythonInsertionPoint(sample.frames, modules_);
                if (insertion != sample.frames.size()) {
                    sample.frames.insert(sample.frames.begin() + static_cast<std::ptrdiff_t>(insertion),
                                         python_snapshot.depth, FrameKey{});
                    for (std::size_t i = 0; i < python_snapshot.depth; ++i) {
                        sample.frames[insertion + i] =
                            pythonFrameKey(python_snapshot.codes[python_snapshot.depth - 1 - i]);
                    }
                    python_attributed = true;
                }
                else {
                    python_boundary_miss = true;
                }
            }
            if (python_provider != nullptr) {
                python_provider->recordSample(python_attributed, python_boundary_miss);
            }

            if (recovery_sink_ && !pending_recovery_module_definitions_.empty()) {
                sample.recovery_module_definitions = pending_recovery_module_definitions_;
            }
            if (enqueueSample(std::move(sample))) {
                pending_recovery_module_definitions_.clear();
            }
            break;
        }
        sampler_heartbeat_.beat();
    }
    sampler_tid_.store(0, std::memory_order_release);
}

}  // namespace spark
