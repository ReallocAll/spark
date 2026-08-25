#ifndef ENDSTONE_SPARK_NATIVE_ATTRIBUTION_H
#define ENDSTONE_SPARK_NATIVE_ATTRIBUTION_H

#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native/alloc/allocation_instrumentation.h"
#include "native/symbol/symbolicate.h"

namespace spark {

class CallTree;
struct ThreadTreeView;

bool isNativeAllocationInstrumentation(std::string_view method_name) noexcept;
bool isNativeAllocationInstrumentationAddress(
    std::uint64_t raw_address, const std::vector<AllocationInstrumentationRange> &ranges) noexcept;
using ResolvedFrameMap = std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash>;

bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved,
                         const std::vector<AllocationInstrumentationRange> &ranges);
bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved);
void filterExecutionTrees(std::vector<ThreadTreeView> &views, std::vector<std::unique_ptr<CallTree>> &owned_trees,
                          const ResolvedFrameMap &resolved);

}  // namespace spark

#endif  // ENDSTONE_SPARK_NATIVE_ATTRIBUTION_H
