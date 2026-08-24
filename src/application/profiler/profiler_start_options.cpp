#include "application/profiler/profiler_start_options.h"

#include <algorithm>
#include <cmath>

namespace spark {

ProfilerStartOptionsResult parseProfilerStartOptions(const Arguments &args)
{
    ProfilerStartOptionsResult result;
    ProfilerOptions &options = result.options;
    options.alloc_live_only = args.boolFlag("alloc-live-only");
    options.alloc = args.boolFlag("alloc") || options.alloc_live_only;
#if !defined(_WIN32) && !defined(__linux__)
    if (options.alloc) {
        result.error = "The native allocation profiler is supported only on Windows x64 and Linux x86-64.";
        return result;
    }
#endif
    options.threads = args.stringFlag("thread");
    options.regex = args.boolFlag("regex");
    if (args.boolFlag("thread") && options.threads.empty()) {
        result.error = "--thread requires a thread name, pattern, or *.";
        return result;
    }
    if (options.regex && options.threads.empty()) {
        result.error = "--regex requires at least one --thread pattern.";
        return result;
    }
    const auto all_selector = std::ranges::find(options.threads, "*");
    if (all_selector != options.threads.end() && (options.regex || options.threads.size() != 1)) {
        result.error = "--thread * cannot be combined with another --thread or --regex.";
        return result;
    }

    auto interval = args.doubleFlag("interval");
    if (args.boolFlag("interval") && !interval) {
        result.error = "The sampling interval must be a finite number.";
        return result;
    }
    if (options.alloc) {
        if (interval && *interval > static_cast<double>(kMaxAllocationIntervalBytes)) {
            result.error =
                "The allocation interval must not exceed " + std::to_string(kMaxAllocationIntervalBytes) + " bytes.";
            return result;
        }
        options.allocation_interval_bytes = interval && *interval != 0.0
                                              ? static_cast<std::int32_t>(std::lround(*interval))
                                              : kDefaultAllocationIntervalBytes;
        options.allocation_interval_bytes = std::max(options.allocation_interval_bytes, 1);
    }
    else {
        if (interval && *interval > kMaxSamplingIntervalMs) {
            result.error = "The sampling interval must not exceed " + std::to_string(kMaxSamplingIntervalMs) + "ms.";
            return result;
        }
        options.interval_ms = interval && *interval != 0.0 ? static_cast<int>(std::lround(*interval)) : 4;
        options.interval_ms = std::max(options.interval_ms, 1);
    }

    auto timeout_flag = args.intFlag("timeout");
    if (args.boolFlag("timeout") && !timeout_flag) {
        result.error = "The timeout must be a whole number of seconds.";
        return result;
    }
    const std::int64_t timeout = timeout_flag.value_or(-1);
    if (timeout_flag && timeout <= 10) {
        result.error = "The timeout is too short for useful results - choose a value over 10 seconds.";
        return result;
    }
    options.timeout_seconds = timeout;

    auto tick_threshold = args.intFlag("only-ticks-over");
    if (args.boolFlag("only-ticks-over") && !tick_threshold) {
        result.error = "The tick threshold must be a whole number of milliseconds.";
        return result;
    }
    if (tick_threshold && *tick_threshold <= 0) {
        result.error = "The tick threshold must be greater than 0ms.";
        return result;
    }
    options.only_ticks_over_ms = tick_threshold.value_or(-1);
    options.ignore_sleeping = args.boolFlag("ignore-sleeping");
    if (args.boolFlag("combine-all") && args.boolFlag("not-combined")) {
        result.error = "--combine-all and --not-combined cannot be used together.";
        return result;
    }
    if (args.boolFlag("combine-all")) {
        options.thread_grouper = ThreadGrouperMode::AsOne;
    }
    else if (args.boolFlag("not-combined")) {
        options.thread_grouper = ThreadGrouperMode::ByName;
    }
    auto comments = args.stringFlag("comment");
    if (!comments.empty()) {
        options.comment = comments.front();
    }
    options.save_to_file = args.boolFlag("save-to-file");
    return result;
}

}  // namespace spark
