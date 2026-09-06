#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native/alloc/windows_dynamic_stack_capture.h"
#endif

#include "native/alloc/allocation_sampler.h"
#include "native/sampler/thread_info.h"

namespace {

using Clock = std::chrono::steady_clock;

void allocationWork(std::size_t operations)
{
    for (std::size_t i = 0; i < operations; ++i) {
        void *pointer = std::malloc(64 + (i & 63));
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
            std::free(pointer);
        }
    }
}

double measure(std::size_t threads, std::size_t operations_per_thread)
{
    const auto start = Clock::now();
    if (threads == 1) {
        allocationWork(operations_per_thread);
    }
    else {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back(allocationWork, operations_per_thread);
        }
        for (std::thread &worker : workers) {
            worker.join();
        }
    }
    const auto elapsed = Clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count();
}

double runTrials(std::size_t threads, std::size_t operations_per_thread)
{
    std::vector<double> trials;
    trials.reserve(5);
    for (int trial = 0; trial < 5; ++trial) {
        trials.push_back(measure(threads, operations_per_thread));
    }
    std::ranges::sort(trials);
    return trials[trials.size() / 2];
}

#ifdef _WIN32
void stackCaptureWork(bool dynamic, std::size_t operations)
{
    void *frames[48]{};
    volatile USHORT depth = 0;
    for (std::size_t i = 0; i < operations; ++i) {
        if (dynamic) {
            depth = spark::captureDynamicAwareStackBackTrace(0, 48, frames, nullptr);
        }
        else {
            depth = ::RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        }
    }
    (void)depth;
}

double measureStackCapture(bool dynamic, std::size_t threads, std::size_t operations_per_thread)
{
    const auto start = Clock::now();
    if (threads == 1) {
        stackCaptureWork(dynamic, operations_per_thread);
    }
    else {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back(stackCaptureWork, dynamic, operations_per_thread);
        }
        for (std::thread &worker : workers) {
            worker.join();
        }
    }
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

double runStackCaptureTrials(bool dynamic, std::size_t threads, std::size_t operations_per_thread)
{
    std::vector<double> trials;
    trials.reserve(5);
    for (int trial = 0; trial < 5; ++trial) {
        trials.push_back(measureStackCapture(dynamic, threads, operations_per_thread));
    }
    std::ranges::sort(trials);
    return trials[trials.size() / 2];
}

struct GatewayClassificationFixture {
    static constexpr std::size_t KGatewayCount = 16;
    struct Gateway {
        void *code = nullptr;
        void *state = nullptr;
        RUNTIME_FUNCTION function{};
    };
    Gateway gateways[KGatewayCount]{};

    bool initialize()
    {
        for (Gateway &gateway : gateways) {
            gateway.code = ::VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            gateway.state = ::VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (gateway.code == nullptr || gateway.state == nullptr) {
                return false;
            }
            auto *code = static_cast<std::uint8_t *>(gateway.code);
            code[0] = 0x49;
            code[1] = 0xBB;
            const auto state_value = reinterpret_cast<std::uint64_t>(gateway.state);
            std::memcpy(code + 2, &state_value, sizeof(state_value));

            auto *state = static_cast<std::uint8_t *>(gateway.state);
            const std::uint64_t magic = spark::dynamic_stack_capture_detail::kPermanentIatGatewayMagic;
            const std::uint32_t abi = spark::dynamic_stack_capture_detail::kPermanentIatGatewayAbiVersion;
            const auto gateway_value = reinterpret_cast<std::uint64_t>(gateway.code);
            std::memcpy(state, &magic, sizeof(magic));
            std::memcpy(state + spark::dynamic_stack_capture_detail::kGatewayStateAbiOffset, &abi, sizeof(abi));
            std::memcpy(state + spark::dynamic_stack_capture_detail::kGatewayStateGatewayOffset, &gateway_value,
                        sizeof(gateway_value));
            gateway.function.BeginAddress = 0;
            gateway.function.EndAddress = 10;
        }
        return true;
    }

    ~GatewayClassificationFixture()
    {
        for (Gateway &gateway : gateways) {
            if (gateway.code != nullptr) {
                ::VirtualFree(gateway.code, 0, MEM_RELEASE);
            }
            if (gateway.state != nullptr) {
                ::VirtualFree(gateway.state, 0, MEM_RELEASE);
            }
        }
    }
};

void gatewayClassificationWork(const GatewayClassificationFixture &fixture, bool cold, std::size_t operations)
{
    volatile std::size_t validated = 0;
    const std::size_t active_gateways = cold ? GatewayClassificationFixture::KGatewayCount : 1;
    for (std::size_t i = 0; i < operations; ++i) {
        const auto &gateway = fixture.gateways[i % active_gateways];
        const auto image_base = reinterpret_cast<DWORD64>(gateway.code);
        validated +=
            spark::dynamic_stack_capture_detail::permanentIatGatewayFrame(image_base + 2, image_base, &gateway.function)
                ? 1U
                : 0U;
    }
    (void)validated;
}

double measureGatewayClassification(const GatewayClassificationFixture &fixture, bool cold, std::size_t threads,
                                    std::size_t operations_per_thread)
{
    const auto start = Clock::now();
    if (threads == 1) {
        gatewayClassificationWork(fixture, cold, operations_per_thread);
    }
    else {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back(gatewayClassificationWork, std::cref(fixture), cold, operations_per_thread);
        }
        for (std::thread &worker : workers) {
            worker.join();
        }
    }
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

double runGatewayClassificationTrials(const GatewayClassificationFixture &fixture, bool cold, std::size_t threads,
                                      std::size_t operations_per_thread)
{
    std::vector<double> trials;
    trials.reserve(5);
    for (int trial = 0; trial < 5; ++trial) {
        trials.push_back(measureGatewayClassification(fixture, cold, threads, operations_per_thread));
    }
    std::ranges::sort(trials);
    return trials[trials.size() / 2];
}
#endif

void printResult(const char *name, std::size_t threads, std::int32_t interval, bool live_only, bool count_only,
                 std::size_t operations_per_thread, double elapsed_ns, std::uint64_t samples, std::uint64_t dropped,
                 std::uint64_t observed_bytes, std::uint64_t hook_calls = 0,
                 std::uint64_t successful_allocation_calls = 0, std::uint64_t sampling_points = 0,
                 std::uint64_t filtered_samples = 0)
{
    const auto operations = static_cast<double>(threads * operations_per_thread);
    std::printf("%s,%zu,%d,%d,%d,%zu,%.0f,%.2f,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", name, threads, interval,
                live_only ? 1 : 0, count_only ? 1 : 0, threads * operations_per_thread, elapsed_ns,
                elapsed_ns / operations, static_cast<unsigned long long>(samples),
                static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(observed_bytes),
                static_cast<unsigned long long>(hook_calls),
                static_cast<unsigned long long>(successful_allocation_calls),
                static_cast<unsigned long long>(sampling_points), static_cast<unsigned long long>(filtered_samples));
}

bool runProfiledCase(spark::AllocationSampler &sampler, const char *name, std::size_t threads, std::int32_t interval,
                     bool live_only, bool count_only, std::size_t operations_per_thread, bool saturated)
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = interval;
    config.session_seed = spark::currentNativeThreadId();
    config.live_only = live_only;
    config.count_only = count_only;
#ifdef SPARK_ALLOCATION_BENCHMARK_CURRENT
    config.aggregator_delay_ms_for_testing = saturated ? 1000 : 0;
#else
    (void)saturated;
#endif

    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "%s: start failed: %s\n", name, error.c_str());
        return false;
    }
    const double elapsed = runTrials(threads, operations_per_thread);
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "%s: stop failed: %s\n", name, error.c_str());
        return false;
    }
#ifdef SPARK_ALLOCATION_BENCHMARK_CURRENT
    const std::uint64_t dropped = sampler.droppedEvents();
#else
    const std::uint64_t dropped = sampler.droppedSamples();
#endif
    printResult(name, threads, interval, live_only, count_only, operations_per_thread, elapsed, sampler.sampleCount(),
                dropped, sampler.observedBytes(), sampler.hookCalls(), sampler.successfulAllocationCalls(),
                sampler.samplingPoints(), sampler.filteredSamples());
    return true;
}

}  // namespace

int main()
{
    constexpr std::size_t k_operations = 200000;
    constexpr std::size_t k_pressure_operations = 16384;
#ifdef _WIN32
    constexpr std::size_t k_stack_capture_operations = 20000;
    constexpr std::size_t k_gateway_classification_operations = 100000;
#endif

    std::printf("case,threads,interval,live_only,count_only,operations_per_trial,median_ns,"
                "ns_per_op,samples_all_trials,dropped_all_trials,observed_bytes,hook_calls,"
                "successful_allocation_calls,sampling_points,filtered_samples\n");
    printResult("unprofiled", 1, 0, false, false, k_operations, runTrials(1, k_operations), 0, 0, 0);
    printResult("unprofiled", 4, 0, false, false, k_operations, runTrials(4, k_operations), 0, 0, 0);
#ifdef _WIN32
    printResult("stack-native", 1, 0, false, false, k_stack_capture_operations,
                runStackCaptureTrials(false, 1, k_stack_capture_operations), 0, 0, 0);
    printResult("stack-dynamic", 1, 0, false, false, k_stack_capture_operations,
                runStackCaptureTrials(true, 1, k_stack_capture_operations), 0, 0, 0);
    printResult("stack-native", 4, 0, false, false, k_stack_capture_operations,
                runStackCaptureTrials(false, 4, k_stack_capture_operations), 0, 0, 0);
    printResult("stack-dynamic", 4, 0, false, false, k_stack_capture_operations,
                runStackCaptureTrials(true, 4, k_stack_capture_operations), 0, 0, 0);

    GatewayClassificationFixture gateway_fixture;
    if (!gateway_fixture.initialize()) {
        std::fprintf(stderr, "gateway classification fixture initialization failed\n");
        return 1;
    }
    const double gateway_warm_1t =
        runGatewayClassificationTrials(gateway_fixture, false, 1, k_gateway_classification_operations);
    printResult("gateway-classify-warm", 1, 0, false, false, k_gateway_classification_operations, gateway_warm_1t, 0, 0,
                0);
    const double gateway_cold_1t =
        runGatewayClassificationTrials(gateway_fixture, true, 1, k_gateway_classification_operations);
    printResult("gateway-classify-cold", 1, 0, false, false, k_gateway_classification_operations, gateway_cold_1t, 0, 0,
                0);
    const double gateway_warm_4t =
        runGatewayClassificationTrials(gateway_fixture, false, 4, k_gateway_classification_operations);
    printResult("gateway-classify-warm", 4, 0, false, false, k_gateway_classification_operations, gateway_warm_4t, 0, 0,
                0);
    const double gateway_cold_4t =
        runGatewayClassificationTrials(gateway_fixture, true, 4, k_gateway_classification_operations);
    printResult("gateway-classify-cold", 4, 0, false, false, k_gateway_classification_operations, gateway_cold_4t, 0, 0,
                0);
#endif

    spark::AllocationSampler sampler;
    if (!runProfiledCase(sampler, "count-only", 1, spark::kDefaultAllocationIntervalBytes, false, true, k_operations,
                         false) ||
        !runProfiledCase(sampler, "count-only", 4, spark::kDefaultAllocationIntervalBytes, false, true, k_operations,
                         false)) {
        std::string ignored;
        sampler.shutdown(ignored);
        return 1;
    }
    printResult("disabled-hooks", 1, 0, false, false, k_operations, runTrials(1, k_operations), 0, 0, 0);
    printResult("disabled-hooks", 4, 0, false, false, k_operations, runTrials(4, k_operations), 0, 0, 0);

    if (!runProfiledCase(sampler, "normal-default", 1, spark::kDefaultAllocationIntervalBytes, false, false,
                         k_operations, false) ||
        !runProfiledCase(sampler, "normal-default", 4, spark::kDefaultAllocationIntervalBytes, false, false,
                         k_operations, false) ||
        !runProfiledCase(sampler, "normal-4k", 1, 4096, false, false, k_operations, false) ||
        !runProfiledCase(sampler, "normal-4k", 4, 4096, false, false, k_operations, false) ||
        !runProfiledCase(sampler, "live-4k", 1, 4096, true, false, k_operations, false) ||
        !runProfiledCase(sampler, "saturated", 4, 1, false, false, k_pressure_operations, true)) {
        std::string ignored;
        sampler.shutdown(ignored);
        return 1;
    }

    std::string error;
    if (!sampler.shutdown(error)) {
        std::fprintf(stderr, "shutdown failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
