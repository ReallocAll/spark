#include "native/symbol/symbol_guess_windows_internal.h"

#ifdef _WIN32

#include <distorm.h>
#include <mnemonics.h>

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>
#include <unordered_set>

namespace spark::symbol_guess::windows {

const FunctionRange *Engine::Impl::fragmentContaining(std::uint32_t root, std::uint32_t rva) const
{
    const FunctionRange *range = containing(rva);
    return range != nullptr && range->root == root ? range : nullptr;
}

std::vector<Engine::Impl::StringCandidate> Engine::Impl::decodeStrings(std::uint32_t root, BuildStats &batch) const
{
    const FunctionRange *root_range = containing(root);
    if (root_range == nullptr || root_range->begin != root || root_range->root != root) {
        return {};
    }
    std::vector<std::uint32_t> work{root};
    if (const auto fragments = chained_fragment_starts.find(root); fragments != chained_fragment_starts.end()) {
        work.insert(work.end(), fragments->second.begin(), fragments->second.end());
    }
    std::unordered_set<std::uint32_t> visited;
    std::map<std::uint32_t, StringCandidate> candidates;

    while (!work.empty()) {
        std::uint32_t cursor = work.back();
        work.pop_back();
        while (const FunctionRange *fragment = fragmentContaining(root, cursor)) {
            if (!visited.insert(cursor).second) {
                break;
            }
            _CodeInfo info{};
            info.codeOffset = cursor;
            info.code = image + cursor;
            info.codeLen = static_cast<int>(fragment->end - cursor);
            info.dt = Decode64Bits;
            info.features = DF_STOP_ON_FLOW_CONTROL | DF_STOP_ON_UNDECODEABLE;
            _DInst instructions[64]{};
            unsigned used = 0;
            const _DecodeResult result = distorm_decompose64(&info, instructions, 64, &used);
            if ((result == DECRES_INPUTERR || result == DECRES_NONE) || used == 0) {
                break;
            }
            bool stop = false;
            for (unsigned i = 0; i < used; ++i) {
                const _DInst &instruction = instructions[i];
                if (instruction.flags == FLAG_NOT_DECODABLE || instruction.size == 0 ||
                    instruction.addr > std::numeric_limits<std::uint32_t>::max()) {
                    stop = true;
                    break;
                }
                const auto address = static_cast<std::uint32_t>(instruction.addr);
                if (address != cursor && !visited.insert(address).second) {
                    stop = true;
                    break;
                }
                ++batch.decoded_instructions;
                if (instruction.opcode == I_LEA && (instruction.flags & FLAG_RIP_RELATIVE) != 0) {
                    const std::uint64_t target_wide = INSTRUCTION_GET_RIP_TARGET(&instruction);
                    if (target_wide <= std::numeric_limits<std::uint32_t>::max()) {
                        const auto target = static_cast<std::uint32_t>(target_wide);
                        std::string value = readCString(target, 180);
                        const int score = ::spark::symbol_guess::windows::scoreStringHint(value);
                        if (score >= ::spark::symbol_guess::kMinimumStringHintScore) {
                            candidates.try_emplace(
                                target, StringCandidate{.target = target, .value = std::move(value), .score = score});
                        }
                    }
                }

                const unsigned flow = META_GET_FC(instruction.meta);
                if (flow == FC_CND_BRANCH || flow == FC_UNC_BRANCH) {
                    for (const _Operand &operand : instruction.ops) {
                        if (operand.type == O_PC) {
                            const std::uint64_t target_wide = INSTRUCTION_GET_TARGET(&instruction);
                            if (target_wide <= std::numeric_limits<std::uint32_t>::max() &&
                                fragmentContaining(root, static_cast<std::uint32_t>(target_wide)) != nullptr) {
                                work.push_back(static_cast<std::uint32_t>(target_wide));
                            }
                            break;
                        }
                    }
                }
                cursor = address + instruction.size;
                if (flow == FC_RET || flow == FC_SYS || flow == FC_UNC_BRANCH || flow == FC_INT || flow == FC_HLT) {
                    stop = true;
                }
            }
            if (stop || fragmentContaining(root, cursor) == nullptr) {
                break;
            }
        }
    }

    std::vector<StringCandidate> out;
    out.reserve(candidates.size());
    for (auto &[target, candidate] : candidates) {
        out.push_back(std::move(candidate));
    }
    std::ranges::sort(out, [](const StringCandidate &a, const StringCandidate &b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.value != b.value) {
            return a.value < b.value;
        }
        return a.target < b.target;
    });
    batch.string_candidates += out.size();
    return out;
}

void Engine::Impl::scanCandidateReferences(const std::unordered_set<std::uint32_t> &targets,
                                           std::unordered_map<std::uint32_t, std::set<std::uint32_t>> &references) const
{
    for (const Section &section : sections) {
        if (!section.executable || section.end - section.begin < 7) {
            continue;
        }
        for (std::uint32_t rva = section.begin; rva <= section.end - 7; ++rva) {
            const std::uint8_t *code = image + rva;
            if (code[0] < 0x48 || code[0] > 0x4f || code[1] != 0x8d || (code[2] & 0xc7) != 0x05) {
                continue;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, code + 3, sizeof(displacement));
            const std::int64_t target_wide = static_cast<std::int64_t>(rva) + 7 + displacement;
            if (target_wide < 0 || std::cmp_greater(target_wide, std::numeric_limits<std::uint32_t>::max())) {
                continue;
            }
            const auto target = static_cast<std::uint32_t>(target_wide);
            if (!targets.contains(target)) {
                continue;
            }
            if (const FunctionRange *function = containing(rva)) {
                references[target].insert(function->root);
            }
        }
    }
}

}  // namespace spark::symbol_guess::windows

#endif
