#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/profiler/profiler.h"
#include "native/sampler/sampler.h"
#include "native/sampler/thread_info.h"
#include "native/sampler/types.h"
#include "proto/sampler_data.h"
#include "selftest_internal.h"
#include "spark_constants.h"

namespace spark {

void SamplerTestAccess::setSamplerThreadHook(Sampler &sampler, std::function<void()> hook)
{
    {
        sampler.sampler_thread_hook_ = std::move(hook);
    }
}

bool SamplerTestAccess::workersJoinable(const Sampler &sampler)
{
    {
        return sampler.sampler_thread_.joinable() || sampler.aggregator_thread_.joinable();
    }
}

namespace {

std::size_t countNodes(const CallTree::Node &node)  // NOLINT(misc-no-recursion)
{
    std::size_t count = node.children.size();
    for (const auto &[key, child] : node.children) {
        count += countNodes(*child);
    }
    return count;
}

}  // namespace

bool SamplerTestAccess::verifyContinuousHistory()
{
    Sampler continuous;
    const std::int32_t base_window = profiling_window::windowNow();
    continuous.resetSession();
    Sample sample;
    sample.weight = 1;
    for (std::int32_t offset = 0; offset <= 120; ++offset) {
        const auto window = static_cast<std::int32_t>(static_cast<std::int64_t>(base_window) + offset);
        sample.thread_id = static_cast<std::uint64_t>(window) + 1;
        sample.thread_name = "Rotating thread";
        sample.frames = {{.module = 0,
                          .rva = static_cast<std::uint64_t>(window) + 1,
                          .raw_address = static_cast<std::uint64_t>(window) + 1}};
        sample.window = window;
        continuous.acceptSample(sample);
        if (offset == 0 || offset == 120) {
            sample.thread_id = 10'000;
            sample.thread_name = "Spanning thread";
            sample.frames = {{.module = 1,
                              .rva = static_cast<std::uint64_t>(offset == 0 ? 100 : 101),
                              .raw_address = static_cast<std::uint64_t>(offset == 0 ? 100 : 101)},
                             {.module = 1, .rva = 200, .raw_address = 200},
                             {.module = 1, .rva = 300, .raw_address = 300}};
            continuous.acceptSample(sample);
        }
        continuous.window_ticks_[window] = WindowTickStats{.ticks = 1, .mspt_sum = 1.0, .mspt_max = 1.0};
        continuous.maybePruneTickHistory(window);
    }
    for (std::uint64_t tick = 0; tick < 10000; ++tick) {
        continuous.recordTickDecision(tick, true);
    }
    const auto &root = continuous.tree_.root();
    const auto spanning = continuous.thread_trees_.find(10'000);
    const FrameKey expired_unique{.module = 0, .rva = 1, .raw_address = 1};
    const FrameKey expired_nested{.module = 1, .rva = 100, .raw_address = 100};
    const auto keys = collectFrameKeys(continuous.tree_);
    std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> resolved;
    resolved[expired_unique] = {.class_name = "test", .method_name = "expired-only-frame"};
    resolved[expired_nested] = {.class_name = "test", .method_name = "expired-nested-frame"};
    ProfileMetadata metadata;
    const std::string payload = buildSamplerData(metadata, continuous.tree_, resolved);
    if (root.times.size() != 61 ||
        root.times.begin()->first != static_cast<std::int32_t>(static_cast<std::int64_t>(base_window) + 60) ||
        root.times.rbegin()->first != static_cast<std::int32_t>(static_cast<std::int64_t>(base_window) + 120) ||
        continuous.window_ticks_.size() != 61 || continuous.sampleCount() != 62 ||
        continuous.thread_trees_.size() != 62 || spanning == continuous.thread_trees_.end() ||
        spanning->second.tree.root().times.size() != 1 || countNodes(root) != 64 ||
        countNodes(spanning->second.tree.root()) != 3 || std::ranges::find(keys, expired_unique) != keys.end() ||
        std::ranges::find(keys, expired_nested) != keys.end() ||
        payload.find("expired-only-frame") != std::string::npos ||
        payload.find("expired-nested-frame") != std::string::npos ||
        continuous.tick_decisions_.size() > Sampler::kTickDecisionCapacity) {
        return false;
    }

    std::printf("continuous history: windows=121 retained=61 nodes=127 retained=64 "
                "thread_roots=122 retained=62\n");

    Sampler foreground;
    for (std::int32_t offset = 0; offset <= 120; ++offset) {
        const auto window = static_cast<std::int32_t>(static_cast<std::int64_t>(base_window) + offset);
        sample.thread_id = static_cast<std::uint64_t>(window) + 1;
        sample.frames = {{.module = 0,
                          .rva = static_cast<std::uint64_t>(window) + 1,
                          .raw_address = static_cast<std::uint64_t>(window) + 1}};
        sample.window = window;
        foreground.acceptSample(sample);
    }
    return foreground.tree_.root().times.size() == 61 && foreground.sampleCount() == 61 &&
           countNodes(foreground.tree_.root()) == 61 && foreground.thread_trees_.size() == 61;
}

}  // namespace spark

namespace spark::selftest {

bool verifySessionIsolation(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    spark::SamplerConfig config;
    config.interval_us = 1000;
    config.ignore_sleeping = false;

    spark::Sampler sampler;
    sampler.setTarget(worker_tid);
    if (!sampler.start(config)) {
        std::fprintf(stderr, "session isolation: sampler start failed\n");
        return false;
    }
    // Wait for at least one sample before the observation loop; first capture is non-deterministic.
    waitForCondition([&] { return sampler.sampleCount() > 0; }, 2s);
    std::uint64_t observed_samples = 0;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(1ms);
        sampler.onTick(50.0);
        std::uint64_t current_samples = sampler.sampleCount();
        if (current_samples < observed_samples) {
            std::fprintf(stderr, "session isolation: live sample count moved backwards\n");
            sampler.stop();
            return false;
        }
        observed_samples = current_samples;
    }
    sampler.stop();
    if (sampler.sampleCount() == 0 || sampler.tree().sampleCount() == 0 || sampler.modules().size() == 0 ||
        sampler.numberOfTicks() != 50 || sampler.windowTicks().empty()) {
        std::fprintf(stderr, "session isolation: first sampler session did not collect expected state\n");
        return false;
    }

    sampler.setTarget(0);
    if (!sampler.start(config)) {
        std::fprintf(stderr, "session isolation: sampler restart failed\n");
        return false;
    }
    sampler.stop();
    if (sampler.sampleCount() != 0 || sampler.modules().size() != 1 || sampler.numberOfTicks() != 0 ||
        !sampler.windowTicks().empty()) {
        std::fprintf(stderr, "session isolation: stop/restart retained sampler state\n");
        return false;
    }

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.ignore_sleeping = false;
    std::string error;
    if (!profiler.start(options, worker_tid, error)) {
        std::fprintf(stderr, "session isolation: profiler start failed: %s\n", error.c_str());
        return false;
    }
    std::this_thread::sleep_for(50ms);
    profiler.cancel();
    if (profiler.sampleCount() == 0) {
        std::fprintf(stderr, "session isolation: cancelled session did not collect a sample\n");
        return false;
    }

    if (!profiler.start(options, 0, error)) {
        std::fprintf(stderr, "session isolation: profiler restart failed: %s\n", error.c_str());
        return false;
    }
    spark::ExportContext context;
    profiler.stop(context);
    if (profiler.sampleCount() != 0) {
        std::fprintf(stderr, "session isolation: cancel/restart retained samples\n");
        return false;
    }

    return true;
}

bool verifyStopResponsiveness()
{
    using namespace std::chrono_literals;

    spark::SamplerConfig config;
    config.interval_us = 5'000'000;
    spark::Sampler sampler;
    sampler.setTarget(0);
    if (!sampler.start(config)) {
        std::fprintf(stderr, "stop responsiveness: sampler start failed\n");
        return false;
    }
    if (sampler.start(config)) {
        std::fprintf(stderr, "stop responsiveness: running sampler started twice\n");
        sampler.stop();
        return false;
    }
    std::this_thread::sleep_for(10ms);
    auto before = std::chrono::steady_clock::now();
    sampler.stop();
    auto elapsed = std::chrono::steady_clock::now() - before;
    if (elapsed >= 500ms) {
        std::fprintf(stderr, "stop responsiveness: stop took too long\n");
        return false;
    }

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.interval_ms = spark::kMaxSamplingIntervalMs + 1;
    std::string error;
    if (profiler.start(options, 0, error)) {
        std::fprintf(stderr, "stop responsiveness: excessive interval was accepted\n");
        profiler.cancel();
        return false;
    }

    options.interval_ms = 4;
    options.timeout_seconds = std::numeric_limits<std::int64_t>::max();
    if (profiler.start(options, 0, error)) {
        std::fprintf(stderr, "stop responsiveness: overflowing timeout was accepted\n");
        profiler.cancel();
        return false;
    }
    options.interval_ms = 1;
    options.timeout_seconds = -1;
    if (!profiler.start(options, 0, error)) {
        std::fprintf(stderr, "stop responsiveness: profiler did not recover after failed start\n");
        return false;
    }
    profiler.cancel();
    return true;
}

bool verifyTickFiltering(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    spark::SamplerConfig config;
    config.interval_us = 1000;
    config.ignore_sleeping = false;
    config.only_ticks_over_ms = 10;

    spark::Sampler sampler;
    sampler.setTarget(worker_tid);
    if (!sampler.start(config)) {
        std::fprintf(stderr, "tick filtering: fast session start failed\n");
        return false;
    }
    // Wait for the sampler to complete at least one capture iteration before
    // emitting a tick event, so that buffered samples exist to filter.
    {
        const auto seq0 = sampler.samplerHeartbeat().sequence.load();
        waitForCondition([&] { return sampler.samplerHeartbeat().sequence.load() > seq0; }, 2s);
    }
    sampler.onTick(1.0);
    sampler.stop();
    if (sampler.sampleCount() != 0) {
        std::fprintf(stderr, "tick filtering: fast tick samples were retained\n");
        return false;
    }

    if (!sampler.start(config)) {
        std::fprintf(stderr, "tick filtering: slow session start failed\n");
        return false;
    }
    {
        const auto seq0 = sampler.samplerHeartbeat().sequence.load();
        waitForCondition([&] { return sampler.samplerHeartbeat().sequence.load() > seq0; }, 2s);
    }
    sampler.onTick(50.0);
    sampler.stop();
    if (sampler.sampleCount() == 0 || sampler.tree().sampleCount() == 0) {
        std::fprintf(stderr, "tick filtering: slow tick samples were not retained\n");
        return false;
    }
    return true;
}

#if defined(_WIN32) || defined(__linux__)
bool verifyThreadDiscovery()
{
    const std::uint64_t current = spark::currentNativeThreadId();
    std::vector<spark::ThreadInfo> threads = spark::enumerateProcessThreads();
    if (current == 0 || threads.empty()) {
        std::fprintf(stderr, "thread discovery: current process threads were not enumerated\n");
        return false;
    }

    bool found_current = false;
    std::uint64_t previous = 0;
    for (const spark::ThreadInfo &thread : threads) {
        if (thread.id == 0 || thread.name.empty() || (previous != 0 && thread.id <= previous)) {
            std::fprintf(stderr, "thread discovery: invalid or unordered thread entry\n");
            return false;
        }
        found_current = found_current || thread.id == current;
        previous = thread.id;
    }
    if (!found_current) {
        std::fprintf(stderr, "thread discovery: current thread is missing\n");
        return false;
    }
    return true;
}

bool verifyAllThreadSampling()
{
    using namespace std::chrono_literals;

    std::atomic<bool> keep_workers_running{true};
    std::atomic<std::uint64_t> first_progress{0};
    std::atomic<std::uint64_t> second_progress{0};
    auto busy_worker = [&](std::atomic<std::uint64_t> &progress) {
        while (keep_workers_running.load(std::memory_order_relaxed)) {
            progress.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread first_worker(busy_worker, std::ref(first_progress));
    std::thread second_worker(busy_worker, std::ref(second_progress));
    while (first_progress.load(std::memory_order_relaxed) == 0 ||
           second_progress.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }
    auto stop_workers = [&] {
        keep_workers_running.store(false, std::memory_order_relaxed);
        first_worker.join();
        second_worker.join();
    };

    spark::SamplerConfig config;
    config.interval_us = 2000;
    config.ignore_sleeping = false;
    config.all_threads = true;

    spark::Sampler sampler;
    if (!sampler.start(config)) {
        std::fprintf(stderr, "all-thread sampling: sampler start failed\n");
        stop_workers();
        return false;
    }
    // Hosted Windows runners can spend most of a short observation interval
    // inside one expensive StackWalk64 attempt.  Poll until at least two
    // thread trees are captured, with a generous deadline for slow hosts.
    waitForCondition([&] { return sampler.threadTrees().size() >= 2 && sampler.sampleCount() > 0; }, 10s);
    sampler.stop();
    stop_workers();

    if (sampler.threadTrees().size() < 2 || sampler.sampleCount() == 0) {
        std::fprintf(stderr, "all-thread sampling: fewer than two process threads were captured\n");
        return false;
    }
    if (sampler.sampleCount() > 750) {
        std::fprintf(stderr, "all-thread sampling: stack-walk interval budget was exceeded\n");
        return false;
    }
    std::uint64_t thread_weight_sum = 0;
    for (const auto &[id, thread] : sampler.threadTrees()) {
        const std::uint64_t weight_us = thread.tree.sampleCount();
        if (id == 0 || thread.thread_name.empty() || thread.tree.empty()) {
            std::fprintf(stderr, "all-thread sampling: invalid per-thread call tree\n");
            return false;
        }
        thread_weight_sum += weight_us;
    }
    // A bounded round-robin sweep can capture a thread only once on a slow host.
    // Preserve the invariant that every accepted weight reaches both tree views
    // without requiring a minimum number of scheduling turns within 200ms.
    if (sampler.tree().sampleCount() != thread_weight_sum) {
        std::fprintf(stderr, "all-thread sampling: combined tree lost elapsed-time weight\n");
        return false;
    }
    return true;
}

std::string escapeRegex(const std::string &text)
{
    std::string escaped;
    for (char ch : text) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(ch) != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

bool verifySelectedThreadSampling(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    const std::vector<spark::ThreadInfo> discovered = spark::enumerateProcessThreads();
    auto worker = std::ranges::find_if(
        discovered, [worker_tid](const spark::ThreadInfo &thread) { return thread.id == worker_tid; });
    if (worker == discovered.end()) {
        std::fprintf(stderr, "selected-thread sampling: worker thread was not discovered\n");
        return false;
    }

    spark::Sampler sampler;
    spark::SamplerConfig invalid;
    invalid.regex_threads = true;
    invalid.thread_patterns = {"["};
    if (sampler.start(invalid) || sampler.lastError().find("invalid thread name regex") == std::string::npos) {
        std::fprintf(stderr, "selected-thread sampling: invalid regex did not fail cleanly\n");
        sampler.stop();
        return false;
    }

    spark::SamplerConfig exact;
    exact.interval_us = 2000;
    exact.ignore_sleeping = false;
    exact.thread_patterns = {worker->name};
    std::transform(exact.thread_patterns.front().begin(), exact.thread_patterns.front().end(),
                   exact.thread_patterns.front().begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (!sampler.start(exact)) {
        std::fprintf(stderr, "selected-thread sampling: exact-name start failed: %s\n", sampler.lastError().c_str());
        return false;
    }
    // The sampler thread needs time to start, enumerate process threads, and
    // complete at least one stack-walk capture.  Poll for a sample instead of
    // relying on a fixed sleep that may expire before the first capture.
    waitForCondition([&] { return sampler.sampleCount() > 0; }, 2s);
    sampler.stop();
    if (sampler.threadTrees().empty()) {
        std::fprintf(stderr, "selected-thread sampling: exact-name selector captured no threads\n");
        return false;
    }
    for (const auto &[id, thread] : sampler.threadTrees()) {
        if (!thread.thread_name.starts_with(worker->name + " (#")) {
            std::fprintf(stderr, "selected-thread sampling: exact-name selector captured an unexpected thread\n");
            return false;
        }
    }

    spark::SamplerConfig regex = exact;
    regex.regex_threads = true;
    regex.thread_patterns = {escapeRegex(worker->name)};
    if (!sampler.start(regex)) {
        std::fprintf(stderr, "selected-thread sampling: regex start failed: %s\n", sampler.lastError().c_str());
        return false;
    }
    waitForCondition([&] { return sampler.sampleCount() > 0; }, 2s);
    sampler.stop();
    if (sampler.threadTrees().empty()) {
        std::fprintf(stderr, "selected-thread sampling: regex selector captured no threads\n");
        return false;
    }
    return true;
}
#endif

}  // namespace spark::selftest
