#ifndef ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_INTERNAL_H
#define ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_INTERNAL_H

#include "native/symbol/symbol_guess_windows.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace spark::symbol_guess::windows {

namespace detail {

struct Section {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    bool executable = false;
};

struct CompleteObjectLocator {
    std::uint32_t signature;
    std::uint32_t offset;
    std::uint32_t cd_offset;
    std::uint32_t type_descriptor;
    std::uint32_t class_descriptor;
    std::uint32_t self;
};

struct ClassHierarchyDescriptor {
    std::uint32_t signature;
    std::uint32_t attributes;
    std::uint32_t base_count;
    std::uint32_t base_array;
};

struct StringCandidate {
    std::uint32_t target = 0;
    std::string value;
    int score = 0;
};

bool checkedAdd(std::uint32_t a, std::uint32_t b, std::uint32_t &out);
std::string classNameFromTypeDescriptor(std::string_view mangled);

}  // namespace detail

struct Engine::Impl {
    using Section = detail::Section;
    using CompleteObjectLocator = detail::CompleteObjectLocator;
    using ClassHierarchyDescriptor = detail::ClassHierarchyDescriptor;
    using StringCandidate = detail::StringCandidate;

    const std::uint8_t *image = nullptr;
    std::size_t mapped_size = 0;
    std::uint64_t load_address = 0;
    std::uint32_t image_size = 0;
    IMAGE_DATA_DIRECTORY exception{};
    std::vector<Section> sections;
    std::vector<FunctionRange> ranges;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> chained_fragment_starts;
    std::unordered_map<std::uint32_t, TypedLabel> vtable_labels;
    mutable std::mutex mutex;
    BuildStats stats;

    template <typename T>
    bool readRaw(std::size_t offset, T &out) const
    {
        if (offset > mapped_size || sizeof(T) > mapped_size - offset) {
            return false;
        }
        std::memcpy(&out, image + offset, sizeof(T));
        return true;
    }

    const Section *sectionContaining(std::uint32_t rva, std::uint32_t length = 1) const
    {
        auto it = std::upper_bound(sections.begin(), sections.end(), rva,  // NOLINT(modernize-use-ranges)
                                   [](std::uint32_t value, const Section &section) { return value < section.begin; });
        if (it == sections.begin()) {
            return nullptr;
        }
        --it;
        return rva >= it->begin && rva < it->end && length <= it->end - rva ? &*it : nullptr;
    }

    template <typename T>
    bool read(std::uint32_t rva, T &out) const
    {
        if (sectionContaining(rva, static_cast<std::uint32_t>(sizeof(T))) == nullptr) {
            return false;
        }
        std::memcpy(&out, image + rva, sizeof(T));
        return true;
    }

    bool toRva(std::uint64_t pointer, std::uint32_t &rva) const
    {
        if (pointer < load_address || pointer - load_address >= image_size ||
            pointer - load_address > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        rva = static_cast<std::uint32_t>(pointer - load_address);
        return sectionContaining(rva) != nullptr;
    }

    std::string readCString(std::uint32_t rva, std::uint32_t maximum) const;
    bool parseHeaders();
    bool validFunction(const RUNTIME_FUNCTION &function) const;
    std::optional<std::uint32_t> chainRoot(const RUNTIME_FUNCTION &start) const;
    void collectFunctions();
    const FunctionRange *containing(std::uint64_t rva) const;
    std::optional<std::pair<std::string, CompleteObjectLocator>> validateCol(std::uint32_t rva) const;
    std::optional<std::uint32_t> directThunkTarget(std::uint32_t rva);
    void collectVtables();
    const FunctionRange *fragmentContaining(std::uint32_t root, std::uint32_t rva) const;
    std::vector<StringCandidate> decodeStrings(std::uint32_t root, BuildStats &batch) const;
    void scanCandidateReferences(const std::unordered_set<std::uint32_t> &targets,
                                 std::unordered_map<std::uint32_t, std::set<std::uint32_t>> &references) const;
    void updateApproximateBytes();
    void initialize();
    std::unordered_map<std::uint64_t, TypedLabel> guess(std::span<const std::uint64_t> rvas);
};

}  // namespace spark::symbol_guess::windows

#endif

#endif  // ENDSTONE_SPARK_SYMBOL_GUESS_WINDOWS_INTERNAL_H
