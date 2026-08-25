#include "native/alloc/allocation_instrumentation.h"

#include <cstdint>

namespace spark {
namespace {

#if defined(__linux__) && defined(__x86_64__)
extern "C" const char __spark_allocation_hooks_start[];
extern "C" const char __spark_allocation_hooks_end[];
#endif

}  // namespace

std::vector<AllocationInstrumentationRange> allocationInstrumentationRanges()
{
#if defined(__linux__) && defined(__x86_64__)
    const auto begin = reinterpret_cast<std::uintptr_t>(__spark_allocation_hooks_start);
    const auto end = reinterpret_cast<std::uintptr_t>(__spark_allocation_hooks_end);
    if (begin != 0 && begin < end) {
        return {{.begin = static_cast<std::uint64_t>(begin), .end = static_cast<std::uint64_t>(end)}};
    }
#endif
    return {};
}

}  // namespace spark
