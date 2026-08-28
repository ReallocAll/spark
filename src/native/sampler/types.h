#ifndef ENDSTONE_SPARK_TYPES_H
#define ENDSTONE_SPARK_TYPES_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

using ModuleId = std::uint32_t;
inline constexpr ModuleId kPythonFrameModule = 0xfffffffeu;
inline constexpr ModuleId kInvalidModule = 0xffffffffu;

// Sentinel path used by bounded ModuleTable for modules that exceed the cap.
// Must be journaled as ModuleDef(0, ...) so recovery can remap overflow frames.
inline constexpr std::string_view kOtherModulesSentinel = "<other modules>";

struct RecoveryModuleDefinition {
    ModuleId module_id = kInvalidModule;
    std::string path;
};

// A stack frame identified by its module and module-relative address (RVA). This
// is stable across the run and is all we need to aggregate; symbol resolution is
// deferred to export time. kPythonFrameModule is reserved for transient Python
// CodeId frames and is never written to crash-recovery journals.
struct FrameKey {
    ModuleId module = kInvalidModule;
    std::uint64_t rva = 0;
    std::uint64_t raw_address = 0;  // runtime PC (for cpptrace resolution); not part of identity

    bool operator==(const FrameKey &o) const noexcept { return module == o.module && rva == o.rva; }
};

struct FrameKeyHash {
    std::size_t operator()(const FrameKey &k) const noexcept
    {
        std::uint64_t h = k.rva + 0x9e3779b97f4a7c15ULL + (static_cast<std::uint64_t>(k.module) << 1);
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebULL;
        h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }
};

// Interns module path strings to small ids.
class ModuleTable {
public:
    explicit ModuleTable(std::size_t maximum_paths = 0) : maximum_paths_(maximum_paths)
    {
        if (maximum_paths_ != 0) {
            paths_.emplace_back(kOtherModulesSentinel);
        }
    }

    ModuleId intern(std::string_view path)
    {
        for (ModuleId i = 0; i < paths_.size(); ++i) {
            if (paths_[i] == path) {
                return i;
            }
        }
        if (maximum_paths_ != 0 && paths_.size() >= maximum_paths_) {
            return 0;
        }
        paths_.emplace_back(path);
        return static_cast<ModuleId>(paths_.size() - 1);
    }

    const std::string &path(ModuleId id) const
    {
        if (id < paths_.size()) {
            return paths_[id];
        }
        static const std::string synthetic_or_unknown = "<synthetic frame>";
        return synthetic_or_unknown;
    }

    std::size_t size() const { return paths_.size(); }

private:
    std::size_t maximum_paths_ = 0;
    std::vector<std::string> paths_;
};

// One captured stack, ordered leaf (index 0) -> root.
struct Sample {
    std::vector<FrameKey> frames;
    // Module definitions first observed by the sampler thread. They travel
    // with the next successfully queued sample so the aggregator can journal
    // definitions before any sample that references them.
    std::vector<RecoveryModuleDefinition> recovery_module_definitions;
    std::uint64_t thread_id = 0;
    std::uint64_t os_thread_id = 0;
    std::string thread_name;
    std::int32_t window = 0;
    std::uint64_t tick_id = 0;
    std::uint64_t weight = 1;  // execution elapsed microseconds or allocation bytes
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_TYPES_H
