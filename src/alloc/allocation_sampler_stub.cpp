#include "alloc/allocation_sampler.h"

#include <memory>
#include <utility>

namespace spark {

struct AllocationSampler::Impl {
    CallTree tree;
    ModuleTable modules;
    std::map<std::int32_t, WindowTickStats> windows;
};

AllocationSampler::AllocationSampler() : impl_(std::make_unique<Impl>())
{
}

AllocationSampler::~AllocationSampler() = default;

bool AllocationSampler::start(const AllocationSamplerConfig &, std::string &error)
{
    error = "native allocation profiling is currently supported only on Windows";
    return false;
}

bool AllocationSampler::stop(std::string &error)
{
    error.clear();
    return true;
}

bool AllocationSampler::shutdown(std::string &error)
{
    error.clear();
    return true;
}

void AllocationSampler::onTick(double)
{
}

const CallTree &AllocationSampler::tree() const
{
    return impl_->tree;
}

const ModuleTable &AllocationSampler::modules() const
{
    return impl_->modules;
}

const std::map<std::int32_t, WindowTickStats> &AllocationSampler::windowTicks() const
{
    return impl_->windows;
}

std::uint64_t AllocationSampler::numberOfTicks() const
{
    return 0;
}

std::uint64_t AllocationSampler::sampleCount() const
{
    return 0;
}

std::uint64_t AllocationSampler::sampledBytes() const
{
    return 0;
}

std::uint64_t AllocationSampler::observedBytes() const
{
    return 0;
}

std::uint64_t AllocationSampler::droppedSamples() const
{
    return 0;
}

bool AllocationSampler::running() const
{
    return false;
}

bool AllocationSampler::hooksInstalled() const
{
    return false;
}

bool AllocationSampler::failure(std::string &error) const
{
    error.clear();
    return false;
}

}  // namespace spark
