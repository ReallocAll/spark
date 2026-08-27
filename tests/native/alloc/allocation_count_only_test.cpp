#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "native/alloc/allocation_sampler.h"
#include "native/sampler/thread_info.h"

int main()
{
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    config.session_seed = spark::currentNativeThreadId();
    config.all_threads = true;
    config.count_only = true;

    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "count-only start failed: %s\n", error.c_str());
        return 1;
    }

    const std::uint64_t before = sampler.observedBytes();
    constexpr std::size_t k_bytes = 32768;
    void *allocation = std::malloc(k_bytes);
    if (allocation == nullptr) {
        std::fprintf(stderr, "count-only test allocation failed\n");
        return 1;
    }
    static_cast<volatile unsigned char *>(allocation)[0] = 0x5a;
    std::free(allocation);
    const std::uint64_t after = sampler.observedBytes();

    if (after - before != k_bytes) {
        std::fprintf(stderr, "count-only byte delta mismatch: expected=%zu actual=%llu\n", k_bytes,
                     static_cast<unsigned long long>(after - before));
        return 1;
    }
    if (sampler.sampleCount() != 0 || sampler.samplingPoints() != 0 || sampler.droppedSamples() != 0) {
        std::fprintf(stderr, "count-only unexpectedly entered sampling path\n");
        return 1;
    }
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "count-only stop failed: %s\n", error.c_str());
        return 1;
    }
    if (!sampler.shutdown(error)) {
        std::fprintf(stderr, "count-only shutdown failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
