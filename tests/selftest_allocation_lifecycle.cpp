#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/profiler/profiler.h"
#include "selftest_allocation_internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace spark::selftest {

#if defined(_WIN32) || defined(__linux__)
bool verifyAllocationLifecycle()
{
    using namespace std::chrono_literals;

    spark::AllocationSamplerConfig config;
    config.interval_bytes = 256;
    const std::uint64_t server_tid = spark::currentNativeThreadId();
    config.session_seed = server_tid;
    std::string error;

    spark::AllocationSampler sampler;
    if (!runAllocationSession(sampler, config, error) || !sampler.hooksInstalled() ||
        !runAllocationSession(sampler, config, error) || !sampler.hooksInstalled()) {
        return false;
    }
    const auto &capabilities = sampler.hookCapabilities();
    std::size_t active_hooks = 0;
    for (const spark::AllocationHookCapability &capability : capabilities) {
        active_hooks += capability.status == spark::AllocationHookStatus::Active ? 1 : 0;
    }
#ifdef _WIN32
    constexpr std::size_t expected_capabilities = 19;
#else
    constexpr std::size_t expected_capabilities = 7;
#endif
    if (capabilities.size() != expected_capabilities || active_hooks < 3) {
        std::fprintf(stderr, "allocation lifecycle: invalid hook capability report (%zu total, %zu active)\n",
                     capabilities.size(), active_hooks);
        return false;
    }

    config.fail_aggregator_for_testing = true;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation lifecycle: injected-failure start failed: %s\n", error.c_str());
        return false;
    }
    bool failed = false;
    for (int i = 0; i < 1000; ++i) {
        if (sampler.failure(error)) {
            failed = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    std::string stop_error;
    const bool stopped_cleanly = sampler.stop(stop_error);
    if (!failed || stopped_cleanly || sampler.running() ||
        stop_error.find("injected allocation aggregator failure") == std::string::npos) {
        std::fprintf(stderr, "allocation lifecycle: aggregator failure was not surfaced safely: %s\n",
                     stop_error.c_str());
        return false;
    }

    config.fail_aggregator_for_testing = false;
    if (!runAllocationSession(sampler, config, error)) {
        std::fprintf(stderr, "allocation lifecycle: backend did not recover after failure\n");
        return false;
    }

    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation lifecycle: concurrent-stop start failed: %s\n", error.c_str());
        return false;
    }
    std::atomic<bool> allocate{true};
    std::vector<std::thread> concurrent_workers;
    concurrent_workers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        concurrent_workers.emplace_back([&allocate]() {
            while (allocate.load(std::memory_order_relaxed)) {
                void *pointer = std::malloc(256);
                if (pointer != nullptr) {
                    static_cast<volatile unsigned char *>(pointer)[0] = 1;
                    std::free(pointer);
                }
            }
        });
    }
    std::this_thread::sleep_for(20ms);
    const bool concurrent_stop = sampler.stop(error);
    allocate.store(false, std::memory_order_relaxed);
    for (std::thread &worker_thread : concurrent_workers) {
        worker_thread.join();
    }
    if (!concurrent_stop) {
        std::fprintf(stderr, "allocation lifecycle: concurrent stop failed: %s\n", error.c_str());
        return false;
    }

#ifdef _WIN32
    HMODULE fixture = ::LoadLibraryA(SPARK_WINDOWS_ALLOCATION_FIXTURE_PATH);
    using FixtureRun = void (*)(volatile LONG *);
    auto fixture_run = fixture == nullptr
                         ? nullptr
                         : reinterpret_cast<FixtureRun>(::GetProcAddress(fixture, "sparkAllocationFixtureRun"));
    volatile LONG fixture_running = 1;
    std::thread fixture_worker;
    if (fixture_run != nullptr) {

        fixture_worker = std::thread(fixture_run, &fixture_running);
        std::this_thread::sleep_for(20ms);
    }
    const bool shutdown = fixture_run != nullptr && sampler.shutdown(error);
    ::InterlockedExchange(&fixture_running, 0);
    if (fixture_worker.joinable()) {
        fixture_worker.join();
    }
    if (fixture != nullptr) {
        ::FreeLibrary(fixture);
    }
    if (!shutdown || sampler.hooksInstalled()) {
        std::fprintf(stderr, "allocation lifecycle: concurrent hook shutdown failed: %s\n", error.c_str());
        return false;
    }
#else
    if (!sampler.shutdown(error) || sampler.hooksInstalled()) {
        std::fprintf(stderr, "allocation lifecycle: final hook cleanup failed: %s\n", error.c_str());
        return false;
    }
#endif

    // A second instance in the same process models plugin reload: the old
    // active-instance pointer and trampolines must not obstruct new setup.
    spark::AllocationSampler reloaded;
    if (!runAllocationSession(reloaded, config, error) || !reloaded.shutdown(error) || reloaded.hooksInstalled()) {
        std::fprintf(stderr, "allocation lifecycle: reload simulation failed: %s\n", error.c_str());
        return false;
    }

    spark::Profiler failed_profiler;
    if (!failed_profiler.setPersistentAllocationCountingEnabled(true, server_tid, error)) {
        std::fprintf(stderr, "profiler failure state: persistent counter start failed: %s\n", error.c_str());
        return false;
    }
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 256;
    options.fail_allocation_aggregator_for_testing = true;
    if (!failed_profiler.start(options, server_tid, error)) {
        std::fprintf(stderr, "profiler failure state: injected start failed: %s\n", error.c_str());
        return false;
    }
    bool profiler_failed = false;
    for (int i = 0; i < 1000; ++i) {
        if (failed_profiler.backendFailure(error)) {
            profiler_failed = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    if (!profiler_failed || !failed_profiler.cancel(error) || failed_profiler.running()) {
        std::fprintf(stderr, "profiler failure state: failed session did not cancel cleanly: %s\n", error.c_str());
        return false;
    }
    if (!failed_profiler.persistentAllocationCountingEnabled()) {
        std::fprintf(stderr, "profiler failure state: persistent counter did not resume after failed session\n");
        return false;
    }
    const std::uint64_t persistent_before = failed_profiler.persistentAllocationBytes();
    constexpr std::size_t k_persistent_probe_bytes = 4096;
    void *persistent_probe = std::malloc(k_persistent_probe_bytes);
    if (persistent_probe == nullptr) {
        std::fprintf(stderr, "profiler failure state: persistent counter probe allocation failed\n");
        return false;
    }
    static_cast<volatile unsigned char *>(persistent_probe)[0] = 1;
    std::free(persistent_probe);
    const std::uint64_t persistent_after = failed_profiler.persistentAllocationBytes();
    if (persistent_after < persistent_before + k_persistent_probe_bytes) {
        std::fprintf(stderr, "profiler failure state: persistent counter froze after failed session\n");
        return false;
    }
    options.fail_allocation_aggregator_for_testing = false;
    if (!failed_profiler.start(options, server_tid, error) || !exerciseNativeAllocations() ||
        !failed_profiler.stopSampling(error)) {
        std::fprintf(stderr, "profiler failure state: healthy restart failed: %s\n", error.c_str());
        return false;
    }
    if (!failed_profiler.shutdown(error)) {
        std::fprintf(stderr, "allocation capability metadata: shutdown failed: %s\n", error.c_str());
        return false;
    }
    return true;
}
#endif

}  // namespace spark::selftest
