#include "native/symbol/symbol_guess_windows_internal.h"

#ifdef _WIN32

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

namespace spark::symbol_guess::windows {

std::optional<std::pair<std::string, Engine::Impl::CompleteObjectLocator>> Engine::Impl::validateCol(
    std::uint32_t rva) const
{
    CompleteObjectLocator col{};
    if (!read(rva, col) || col.signature != 1 || col.self != rva) {
        return std::nullopt;
    }
    ClassHierarchyDescriptor hierarchy{};
    if (!read(col.class_descriptor, hierarchy) || hierarchy.signature != 0 || hierarchy.base_count == 0 ||
        hierarchy.base_count > 1024) {
        return std::nullopt;
    }
    const Section *base_array = sectionContaining(hierarchy.base_array, hierarchy.base_count * 4U);
    if (base_array == nullptr || base_array->executable) {
        return std::nullopt;
    }
    std::uint32_t first_base = 0;
    std::uint32_t first_type = 0;
    std::uint32_t contained_bases = 0;
    if (!read(hierarchy.base_array, first_base) || !read(first_base, first_type) ||
        !read(first_base + 4, contained_bases) || first_type != col.type_descriptor ||
        contained_bases > hierarchy.base_count) {
        return std::nullopt;
    }
    const std::string mangled = readCString(col.type_descriptor + 16, 256);
    std::string class_name = detail::classNameFromTypeDescriptor(mangled);
    if (class_name.empty()) {
        return std::nullopt;
    }
    return std::pair<std::string, CompleteObjectLocator>{std::move(class_name), col};
}

std::optional<std::uint32_t> Engine::Impl::directThunkTarget(std::uint32_t rva)
{
    const Section *section = sectionContaining(rva);
    if (section == nullptr || !section->executable) {
        return std::nullopt;
    }
    ++stats.thunk_candidates;
    const std::uint32_t available = std::min<std::uint32_t>(section->end - rva, 24);
    if (available < 5) {
        return std::nullopt;
    }
    const std::uint8_t *code = image + rva;
    std::uint32_t prefix = 0;
    if (available >= 9 && code[0] == 0x48 &&
        ((code[1] == 0x83 && (code[2] == 0xe9 || code[2] == 0xc1)) || (code[1] == 0x8d && code[2] == 0x49))) {
        prefix = 4;
    }
    else if (available >= 12 && code[0] == 0x48 &&
             ((code[1] == 0x81 && (code[2] == 0xe9 || code[2] == 0xc1)) || (code[1] == 0x8d && code[2] == 0x89))) {
        prefix = 7;
    }
    if (available < prefix + 5 || code[prefix] != 0xe9) {
        return std::nullopt;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, code + prefix + 1, sizeof(displacement));
    const std::int64_t target = static_cast<std::int64_t>(rva) + prefix + 5 + displacement;
    if (target < 0 || std::cmp_greater(target, std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }
    const FunctionRange *function = containing(static_cast<std::uint32_t>(target));
    if (function == nullptr) {
        return std::nullopt;
    }
    ++stats.thunk_resolved;
    return function->root;
}

void Engine::Impl::collectVtables()
{
    std::unordered_map<std::uint32_t, std::vector<VtableEvidence>> candidates;
    for (const Section &section : sections) {
        if (section.executable) {
            continue;
        }
        const std::uint32_t start = (section.begin + 7U) & ~7U;
        for (std::uint32_t rva = start; rva <= section.end - 16U; rva += 8) {
            std::uint64_t col_pointer = 0;
            std::memcpy(&col_pointer, image + rva, sizeof(col_pointer));
            std::uint32_t col_rva = 0;
            if (!toRva(col_pointer, col_rva)) {
                continue;
            }
            const auto validated = validateCol(col_rva);
            if (!validated) {
                continue;
            }
            const auto &[class_name, col] = *validated;
            ++stats.vtables;
            const std::uint32_t table = rva + 8;
            bool saw_code = false;
            unsigned external_holes = 0;
            for (std::uint32_t slot = 0; slot < 512 && table <= section.end - 8U * (slot + 1U); ++slot) {
                std::uint64_t entry = 0;
                std::memcpy(&entry, image + table + 8U * slot, sizeof(entry));
                std::uint32_t target = 0;
                if (!toRva(entry, target)) {
                    // Permit one external _purecall-like slot after the table has started.
                    if (entry != 0 && saw_code && external_holes++ == 0) {
                        continue;
                    }
                    break;
                }
                if (validateCol(target)) {
                    break;
                }
                const Section *target_section = sectionContaining(target);
                if (target_section == nullptr || !target_section->executable) {
                    break;
                }
                saw_code = true;
                external_holes = 0;
                const FunctionRange *function = containing(target);
                bool via_thunk = false;
                std::uint32_t root = 0;
                if (function != nullptr) {
                    root = function->root;
                }
                else if (const auto thunk = directThunkTarget(target)) {
                    root = *thunk;
                    via_thunk = true;
                }
                else {
                    continue;
                }
                candidates[root].push_back(
                    {.class_name = class_name, .slot = slot, .secondary = col.offset != 0, .via_thunk = via_thunk});
                ++stats.vtable_candidates;
            }
        }
    }
    for (auto &[root, evidence] : candidates) {
        TypedLabel label = ::spark::symbol_guess::windows::chooseVtableLabel(evidence);
        if (label.empty()) {
            ++stats.vtable_conflicts;
            continue;
        }
        vtable_labels.emplace(root, std::move(label));
        ++stats.vtable_labels;
    }
}

}  // namespace spark::symbol_guess::windows

#endif
