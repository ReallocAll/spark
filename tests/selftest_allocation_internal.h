#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "native/alloc/allocation_sampler.h"
#include "native/sampler/thread_info.h"
#include "selftest_internal.h"

namespace spark::selftest {

bool setCurrentThreadName(const char *name);

#if defined(_WIN32) || defined(__linux__)
bool exerciseNativeAllocations();
void allocationBurst(int count = 96);
bool allocationTreesHaveOnly(const spark::AllocationSampler &sampler,
                             const std::vector<std::string_view> &expected_names);
bool runNamedAllocationWorkers(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                               const std::vector<const char *> &names, std::string &error);
bool runAllocationSession(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                          std::string &error);
#endif

}  // namespace spark::selftest
