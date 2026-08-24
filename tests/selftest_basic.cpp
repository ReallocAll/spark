#include <charconv>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/command/arguments.h"
#include "core/stats/tick_monitor.h"
#include "native/symbol/symbolicate.h"
#include "net/bytebin.h"
#include "proto/sampler_data.h"
#include "selftest_internal.h"

namespace spark::selftest {

bool verifyArgumentParsing()
{
    auto integer = [](const std::string &text) {
        spark::Arguments args({"start", "--value", text}, true);
        return args.intFlag("value");
    };
    auto floating = [](const std::string &text) {
        spark::Arguments args({"start", "--value", text}, true);
        return args.doubleFlag("value");
    };

    if (integer("100") != 100 || integer("-1") != 1 || integer("abc") || integer("100abc") ||
        integer("999999999999999999999999999999999999")) {
        std::fprintf(stderr, "argument parsing: integer validation failed\n");
        return false;
    }
    if (floating("1.25") != 1.25 || floating("-1.25") != 1.25 || floating("abc") || floating("100abc") ||
        floating("1e9999") || floating("NaN") || floating("inf")) {
        std::fprintf(stderr, "argument parsing: floating-point validation failed\n");
        return false;
    }

    spark::Arguments missing({"start", "--value"}, true);
    if (!missing.boolFlag("value") || missing.intFlag("value") || missing.doubleFlag("value")) {
        std::fprintf(stderr, "argument parsing: missing value validation failed\n");
        return false;
    }
    const std::vector<std::string> quoted =
        spark::Arguments::tokenize(R"(start --thread "Server thread" --thread '^Worker \d+$' --regex)");
    spark::Arguments selected(quoted, true);
    const std::vector<std::string> threads = selected.stringFlag("thread");
    if (threads.size() != 2 || threads[0] != "Server thread" || threads[1] != R"(^Worker \d+$)" ||
        !selected.boolFlag("regex")) {
        std::fprintf(stderr, "argument parsing: quoted thread selector validation failed\n");
        return false;
    }
    return true;
}

bool verifyUploadFailure()
{
    using namespace std::chrono_literals;

    const auto before = std::chrono::steady_clock::now();
    spark::UploadResult result =
        spark::uploadToBytebin("test", "http://127.0.0.1:1", "application/octet-stream", "spark-selftest");
    const auto elapsed = std::chrono::steady_clock::now() - before;
    if (result.ok || result.error.empty() || elapsed >= 5s) {
        std::fprintf(stderr, "upload failure: invalid target was not rejected promptly\n");
        return false;
    }
    return true;
}

#if defined(_WIN32) || defined(__linux__)

bool verifyTickMonitor()
{
    spark::TickMonitor monitor;
    spark::TickMonitorConfig config;
    config.setup_ticks = 3;
    config.threshold = 100.0;
    if (!monitor.start(config)) {
        std::fprintf(stderr, "tick monitor: valid percentage configuration was rejected\n");
        return false;
    }

    monitor.onTick(10.0);
    monitor.onTick(20.0);
    spark::TickMonitorUpdate setup = monitor.onTick(30.0);
    if (!setup.setup_completed || setup.report || setup.tick != 3 || setup.baseline_ms != 20.0 ||
        setup.setup_min_ms != 10.0 || setup.setup_max_ms != 30.0) {
        std::fprintf(stderr, "tick monitor: baseline calculation failed\n");
        return false;
    }
    if (monitor.onTick(40.0).report) {
        std::fprintf(stderr, "tick monitor: percentage threshold boundary was included\n");
        return false;
    }
    spark::TickMonitorUpdate spike = monitor.onTick(50.0);
    if (!spike.report || spike.tick != 5 || spike.percentage_change != 150.0) {
        std::fprintf(stderr, "tick monitor: percentage spike was not reported\n");
        return false;
    }

    config.mode = spark::TickMonitorMode::Duration;
    config.threshold = 25.0;
    config.setup_ticks = 1;
    if (!monitor.start(config) || !monitor.onTick(10.0).setup_completed || monitor.onTick(25.0).report ||
        !monitor.onTick(25.01).report) {
        std::fprintf(stderr, "tick monitor: duration threshold failed\n");
        return false;
    }
    monitor.stop();
    if (monitor.running() || monitor.onTick(100.0).report) {
        std::fprintf(stderr, "tick monitor: stop did not reset running state\n");
        return false;
    }

    config.threshold = 0.0;
    if (monitor.start(config)) {
        std::fprintf(stderr, "tick monitor: invalid configuration was accepted\n");
        return false;
    }
    return true;
}

bool verifyMultiThreadSerialization()
{
    spark::ModuleTable modules;
    spark::ModuleId module = modules.intern("selftest-module");
    spark::FrameKey first{.module = module, .rva = 0x10, .raw_address = 0x10};
    spark::FrameKey second{.module = module, .rva = 0x20, .raw_address = 0x20};

    spark::CallTree first_tree;
    first_tree.log({first}, 0);
    spark::CallTree second_tree;
    second_tree.log({second}, 1);

    spark::ProfileMetadata metadata;
    metadata.interval = 1000;
    metadata.regex_threads = true;
    metadata.thread_patterns = {"worker-.*"};
    std::unordered_map<spark::FrameKey, spark::ResolvedFrame, spark::FrameKeyHash> resolved;
    resolved[first] = {.class_name = "selftest", .method_name = "firstFrame"};
    resolved[second] = {.class_name = "selftest", .method_name = "secondFrame"};
    std::vector<spark::ThreadTreeView> threads{{.name = "worker-one", .tree = &first_tree},
                                               {.name = "worker-two", .tree = &second_tree}};
    std::string profile = spark::buildSamplerData(metadata, threads, resolved);
    if (profile.find("worker-one") == std::string::npos || profile.find("worker-two") == std::string::npos ||
        profile.find("worker-.*") == std::string::npos || profile.find("firstFrame") == std::string::npos ||
        profile.find("secondFrame") == std::string::npos || spark::collectFrameKeys(threads).size() != 2) {
        std::fprintf(stderr, "multi-thread serialization: thread trees were not preserved\n");
        return false;
    }
    return true;
}

#endif

}  // namespace spark::selftest
