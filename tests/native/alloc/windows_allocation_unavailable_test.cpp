#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native/alloc/allocation_sampler.h"

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "windows allocation backend: %s\n", message);
    return 1;
}

using FixtureOnceFn = void (*)();

bool treeContainsModule(const spark::CallTree::Node &node, const spark::ModuleTable &modules, const char *name)
{
    if (node.key.module != spark::kInvalidModule) {
        const std::string &path = modules.path(node.key.module);
        const std::size_t separator = path.find_last_of("/\\");
        const char *base = separator == std::string::npos ? path.c_str() : path.c_str() + separator + 1;
        if (::_stricmp(base, name) == 0) {
            return true;
        }
    }
    for (const auto &[key, child] : node.children) {
        (void)key;
        if (treeContainsModule(*child, modules, name)) {
            return true;
        }
    }
    return false;
}

bool verifyLateLoadedModuleRefresh(spark::AllocationSampler &sampler)
{
    HMODULE fixture = ::LoadLibraryW(L".\\spark_windows_allocation_fixture.dll");
    if (fixture == nullptr) {
        std::fprintf(stderr, "windows allocation backend: fixture LoadLibraryW failed: %lu\n",
                     static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    FixtureOnceFn once = reinterpret_cast<FixtureOnceFn>(::GetProcAddress(fixture, "sparkAllocationFixtureOnce"));
    if (once == nullptr) {
        ::FreeLibrary(fixture);
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const std::uint64_t before = sampler.hookCalls();
    constexpr int KCalls = 2048;
    for (int i = 0; i < KCalls; ++i) {
        once();
    }
    const std::uint64_t delta = sampler.hookCalls() - before;
    const bool unloaded = ::FreeLibrary(fixture) != FALSE;
    return unloaded && delta >= static_cast<std::uint64_t>(KCalls);
}

bool runSession(spark::AllocationSampler &sampler, std::uint64_t seed, bool verify_refresh)
{
    spark::AllocationSamplerConfig config;
    config.session_seed = seed;
    config.interval_bytes = 4096;

    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "windows allocation backend: start failed: %s\n", error.c_str());
        return false;
    }
    if (!sampler.running() || !sampler.hooksInstalled() || sampler.hookTargetCount() == 0 ||
        sampler.hookCapabilities().empty()) {
        return false;
    }
    if (verify_refresh && !verifyLateLoadedModuleRefresh(sampler)) {
        return false;
    }

    std::vector<std::string> allocations;
    allocations.reserve(256);
    for (int i = 0; i < 256; ++i) {
        allocations.emplace_back(512, static_cast<char>('a' + (i % 26)));
    }
    sampler.onTick(1.0);

    spark::AllocationSnapshot snapshot;
    if (!sampler.snapshot(snapshot, error) || snapshot.sample_count == 0 ||
        treeContainsModule(snapshot.tree.root(), snapshot.modules, "spark_allocation_shim.dll")) {
        return false;
    }

    error = "sentinel";
    if (!sampler.stop(error) || !error.empty() || sampler.running() || !sampler.hooksInstalled()) {
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    spark::AllocationSampler sampler;
    if (!runSession(sampler, 0x12345678ULL, true)) {
        return fail("first session did not complete cleanly");
    }
    if (!runSession(sampler, 0x87654321ULL, false)) {
        return fail("second session did not complete cleanly");
    }

    std::string error = "sentinel";
    if (!sampler.shutdown(error) || !error.empty() || sampler.running() || sampler.hooksInstalled()) {
        std::fprintf(stderr, "windows allocation backend: shutdown failed: %s\n", error.c_str());
        return 1;
    }

    // A process-lifetime shim may remain mapped after shutdown. Any stale IAT
    // entry must therefore be a harmless allocator pass-through after Spark's
    // callback gate has been drained and cleared.
    std::vector<std::string> after_shutdown(64, std::string(256, 'z'));
    if (after_shutdown.size() != 64) {
        return fail("post-shutdown allocator pass-through failed");
    }
    return 0;
}
