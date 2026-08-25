#ifndef ENDSTONE_SPARK_NATIVE_ATTRIBUTION_H
#define ENDSTONE_SPARK_NATIVE_ATTRIBUTION_H

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native/symbol/symbolicate.h"

namespace spark {

class CallTree;
struct ThreadTreeView;

struct NativeInstrumentationRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

bool isNativeAllocationInstrumentation(std::string_view method_name) noexcept;
bool isNativeAllocationInstrumentationAddress(
    std::uint64_t raw_address, std::span<const NativeInstrumentationRange> ranges) noexcept;
using ResolvedFrameMap = std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash>;

bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved);
bool filterExecutionTree(CallTree &filtered, const CallTree &source, const ResolvedFrameMap &resolved,
                         std::span<const NativeInstrumentationRange> instrumentation_ranges);
void filterExecutionTrees(std::vector<ThreadTreeView> &views, std::vector<std::unique_ptr<CallTree>> &owned_trees,
                          const ResolvedFrameMap &resolved);
void filterExecutionTrees(std::vector<ThreadTreeView> &views, std::vector<std::unique_ptr<CallTree>> &owned_trees,
                          const ResolvedFrameMap &resolved,
                          std::span<const NativeInstrumentationRange> instrumentation_ranges);

}  // namespace spark

#endif  // ENDSTONE_SPARK_NATIVE_ATTRIBUTION_H
