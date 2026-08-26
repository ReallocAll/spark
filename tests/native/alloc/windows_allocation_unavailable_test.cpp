#include <cstdio>
#include <string>
#include <vector>

#include "native/alloc/allocation_sampler.h"

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "windows allocation backend: %s\n", message);
    return 1;
}

bool runSession(spark::AllocationSampler &sampler, std::uint64_t seed)
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

    std::vector<std::string> allocations;
    allocations.reserve(256);
    for (int i = 0; i < 256; ++i) {
        allocations.emplace_back(512, static_cast<char>('a' + (i % 26)));
    }
    sampler.onTick(1.0);

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
    if (!runSession(sampler, 0x12345678ULL)) {
        return fail("first session did not complete cleanly");
    }
    if (!runSession(sampler, 0x87654321ULL)) {
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
