#ifndef ENDSTONE_SPARK_ALLOCATION_INSTRUMENTATION_H
#define ENDSTONE_SPARK_ALLOCATION_INSTRUMENTATION_H

#include <cstdint>
#include <vector>

namespace spark {

struct AllocationInstrumentationRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

std::vector<AllocationInstrumentationRange> allocationInstrumentationRanges();

}  // namespace spark

#endif  // ENDSTONE_SPARK_ALLOCATION_INSTRUMENTATION_H
