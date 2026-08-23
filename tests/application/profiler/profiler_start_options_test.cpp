#include <cassert>
#include <string>
#include <vector>

#include "application/profiler/profiler_start_options.h"

namespace {

spark::ProfilerStartOptionsResult parse(std::initializer_list<std::string> tokens)
{
    return spark::parseProfilerStartOptions(spark::Arguments(std::vector<std::string>(tokens)));
}

void expectError(std::initializer_list<std::string> tokens, const std::string &expected)
{
    const auto result = parse(tokens);
    assert(!result.success());
    assert(result.error == expected);
}

}  // namespace

int main()
{
    {
        const auto result = parse({"start"});
        assert(result.success());
        assert(result.options.interval_ms == 4);
        assert(result.options.allocation_interval_bytes == spark::kDefaultAllocationIntervalBytes);
        assert(result.options.timeout_seconds == -1);
        assert(result.options.only_ticks_over_ms == -1);
        assert(!result.options.alloc);
        assert(!result.options.alloc_live_only);
        assert(!result.options.regex);
        assert(!result.options.ignore_sleeping);
        assert(result.options.threads.empty());
        assert(result.options.comment.empty());
        assert(!result.options.save_to_file);
        assert(result.options.creator_name == "Console");
        assert(!result.options.creator_is_player);
        assert(result.options.thread_grouper == spark::ThreadGrouperMode::ByPool);
    }

    {
        const auto result = parse({"start",
                                   "--thread",
                                   "Server thread",
                                   "--thread",
                                   "Worker",
                                   "--regex",
                                   "--interval",
                                   "5.5",
                                   "--timeout",
                                   "11",
                                   "--only-ticks-over",
                                   "3",
                                   "--ignore-sleeping",
                                   "--not-combined",
                                   "--comment",
                                   "first",
                                   "--comment",
                                   "second",
                                   "--save-to-file",
                                   "--unknown",
                                   "value"});
        assert(result.success());
        assert((result.options.threads == std::vector<std::string>{"Server thread", "Worker"}));
        assert(result.options.regex);
        assert(result.options.interval_ms == 6);
        assert(result.options.timeout_seconds == 11);
        assert(result.options.only_ticks_over_ms == 3);
        assert(result.options.ignore_sleeping);
        assert(result.options.thread_grouper == spark::ThreadGrouperMode::ByName);
        assert(result.options.comment == "first");
        assert(result.options.save_to_file);
    }

    {
        const auto result =
            parse({"start", "--alloc-live-only", "--interval", "12.6", "--combine-all", "--comment", "allocation"});
        assert(result.success());
        assert(result.options.alloc);
        assert(result.options.alloc_live_only);
        assert(result.options.allocation_interval_bytes == 13);
        assert(result.options.interval_ms == 4);
        assert(result.options.thread_grouper == spark::ThreadGrouperMode::AsOne);
        assert(result.options.comment == "allocation");
    }

    {
        const auto result = parse({"start", "--alloc", "--interval", "0.4"});
        assert(result.success());
        assert(result.options.allocation_interval_bytes == 1);
    }

    expectError({"start", "--thread"}, "--thread requires a thread name, pattern, or *.");
    expectError({"start", "--regex"}, "--regex requires at least one --thread pattern.");
    expectError({"start", "--thread", "*", "--thread", "Worker"},
                "--thread * cannot be combined with another --thread or --regex.");
    expectError({"start", "--thread", "*", "--regex"},
                "--thread * cannot be combined with another --thread or --regex.");
    expectError({"start", "--interval"}, "The sampling interval must be a finite number.");
    expectError({"start", "--interval", "not-a-number"}, "The sampling interval must be a finite number.");
    expectError({"start", "--interval", "0"}, "The sampling interval must be greater than zero.");
    expectError({"start", "--interval", "-1"}, "The sampling interval must be greater than zero.");
    expectError({"start", "--timeout"}, "The timeout must be a whole number of seconds.");
    expectError({"start", "--timeout", "not-a-number"}, "The timeout must be a whole number of seconds.");
    expectError({"start", "--timeout", "10"},
                "The timeout is too short for useful results - choose a value over 10 seconds.");
    expectError({"start", "--timeout", "-1"},
                "The timeout is too short for useful results - choose a value over 10 seconds.");
    expectError({"start", "--only-ticks-over"}, "The tick threshold must be a whole number of milliseconds.");
    expectError({"start", "--only-ticks-over", "not-a-number"},
                "The tick threshold must be a whole number of milliseconds.");
    expectError({"start", "--only-ticks-over", "0"}, "The tick threshold must be greater than 0ms.");
    expectError({"start", "--only-ticks-over", "-1"}, "The tick threshold must be greater than 0ms.");
    expectError({"start", "--combine-all", "--not-combined"},
                "--combine-all and --not-combined cannot be used together.");

#if !defined(_WIN32) && !defined(__linux__)
    expectError({"start", "--alloc"},
                "The native allocation profiler is supported only on Windows x64 and Linux x86-64.");
#else
    expectError({"start", "--alloc", "--interval", "2147483648"},
                "The allocation interval must not exceed " + std::to_string(spark::kMaxAllocationIntervalBytes) +
                    " bytes.");
    expectError({"start", "--interval", "1001"},
                "The sampling interval must not exceed " + std::to_string(spark::kMaxSamplingIntervalMs) + "ms.");
#endif

    // Earlier validation branches retain precedence over later branches.
    expectError({"start", "--thread", "--regex", "--interval", "bad", "--timeout", "bad"},
                "--thread requires a thread name, pattern, or *.");
    expectError({"start", "--interval", "bad", "--timeout", "bad", "--only-ticks-over", "bad"},
                "The sampling interval must be a finite number.");
    expectError({"start", "--timeout", "bad", "--only-ticks-over", "bad", "--combine-all", "--not-combined"},
                "The timeout must be a whole number of seconds.");
    expectError({"start", "--only-ticks-over", "bad", "--combine-all", "--not-combined"},
                "The tick threshold must be a whole number of milliseconds.");

    return 0;
}
