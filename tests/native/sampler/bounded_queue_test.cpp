#include <cassert>
#include <cstddef>
#include <utility>

#include "native/sampler/sampler.h"

namespace spark {

struct SamplerTestAccess {
    static bool enqueueSample(Sampler &sampler, Sample sample) { return sampler.enqueueSample(std::move(sample)); }
    static void reset(Sampler &sampler) { sampler.resetSession(); }
};

}  // namespace spark

int main()
{
    spark::Sampler sampler;
    spark::Sample sample;
    sample.thread_id = 1;
    sample.thread_name = "bounded-queue-test";
    sample.frames.push_back({.module = 0, .rva = 1, .raw_address = 1});

    bool sample_queue_full = false;
    for (std::size_t i = 0; i < spark::Sampler::sampleQueueCapacity() * 4; ++i) {
        if (!spark::SamplerTestAccess::enqueueSample(sampler, sample)) {
            sample_queue_full = true;
            break;
        }
    }
    assert(sample_queue_full);
    assert(sampler.droppedSamples() == 1);

    for (std::size_t i = 0; i < spark::Sampler::tickQueueCapacity() * 4; ++i) {
        sampler.onTick(1.0);
    }
    assert(sampler.droppedTickEvents() != 0);

    spark::SamplerTestAccess::reset(sampler);
    assert(sampler.droppedSamples() == 0);
    assert(sampler.droppedTickEvents() == 0);
    return 0;
}
