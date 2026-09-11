#ifndef SPARK_ALLOCATION_LAST_ERROR_TESTING
#error "compile the sampler and this test with SPARK_ALLOCATION_LAST_ERROR_TESTING"
#endif

#include <cstdio>
#include <exception>

#include "native/alloc/allocation_sampler.h"

namespace spark::test {
std::size_t windowsAllocationLastErrorCaseCount() noexcept;
}

int main()
{
    try {
        spark::AllocationSampler sampler;
        if (spark::test::windowsAllocationLastErrorCaseCount() != 684) {
            std::fprintf(stderr, "incomplete LastError regression matrix\n");
            return 1;
        }
    }
    catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
