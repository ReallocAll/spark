#include <cstdio>
#include <string>

#include "native/alloc/allocation_sampler.h"

namespace {

constexpr char KExpectedError[] =
    "Windows allocation profiling is temporarily disabled because safe allocator entry patching is unavailable";

int fail(const char *message)
{
    std::fprintf(stderr, "windows allocation unavailable: %s\n", message);
    return 1;
}

}  // namespace

int main()
{
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    std::string error;
    if (sampler.start(config, error) || error != KExpectedError) {
        return fail("start did not return the exact unavailability error");
    }
    if (sampler.running() || sampler.hooksInstalled() || sampler.hookTargetCount() != 0 ||
        !sampler.hookCapabilities().empty()) {
        return fail("failed start left allocation state active");
    }

    error = "sentinel";
    if (!sampler.stop(error) || !error.empty()) {
        return fail("stop was not harmless after unavailable start");
    }
    error = "sentinel";
    if (!sampler.shutdown(error) || !error.empty() || sampler.running() || sampler.hooksInstalled()) {
        return fail("shutdown was not harmless after unavailable start");
    }
    return 0;
}
