#include "native/alloc/windows_stable_entry_experiment.h"

#ifndef _WIN32
#error "windows_stable_entry_alignment_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using spark::stable_entry_experiment::TargetRecord;
using spark::stable_entry_experiment::resolveWindowsAllocatorTargets;

namespace {

void printBytes(const std::uint8_t *bytes, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]);
    }
    std::cout << std::dec;
}

bool readPreEntryBytes(std::uintptr_t entry, std::array<std::uint8_t, 16> &bytes)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (::VirtualQuery(reinterpret_cast<const void *>(entry), &memory, sizeof(memory)) == 0 ||
        memory.BaseAddress == nullptr) {
        return false;
    }
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (entry < begin + bytes.size()) {
        return false;
    }
    std::memcpy(bytes.data(), reinterpret_cast<const void *>(entry - bytes.size()), bytes.size());
    return true;
}

bool allPadding(const std::uint8_t *bytes, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0x90 && bytes[i] != 0xCC) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main()
{
    std::vector<TargetRecord> targets;
    bool dynamic_code_allowed = false;
    std::string error;
    assert(resolveWindowsAllocatorTargets(targets, dynamic_code_allowed, error));
    assert(error.empty());
    assert(targets.size() == 19);

    std::size_t aligned8 = 0;
    std::size_t aligned16 = 0;
    std::size_t rel32_thunks = 0;
    std::size_t pre5_padding = 0;
    std::size_t pre8_padding = 0;
    std::set<std::uintptr_t> unique_entries;
    for (const TargetRecord &target : targets) {
        const bool a8 = (target.resolved_address & 7U) == 0;
        const bool a16 = (target.resolved_address & 15U) == 0;
        const bool rel32_thunk = target.original_bytes[0] == 0xE9;
        aligned8 += a8 ? 1U : 0U;
        aligned16 += a16 ? 1U : 0U;
        rel32_thunks += rel32_thunk ? 1U : 0U;
        unique_entries.insert(target.resolved_address);

        std::array<std::uint8_t, 16> before{};
        const bool have_before = readPreEntryBytes(target.resolved_address, before);
        const bool pad5 = have_before && allPadding(before.data() + before.size() - 5, 5);
        const bool pad8 = have_before && allPadding(before.data() + before.size() - 8, 8);
        pre5_padding += pad5 ? 1U : 0U;
        pre8_padding += pad8 ? 1U : 0U;

        std::cout << target.requested_module << '!' << target.logical_name << " entry=0x" << std::hex
                  << target.resolved_address << std::dec << " align8=" << a8 << " align16=" << a16
                  << " rel32_thunk=" << rel32_thunk << " pre5_padding=" << pad5 << " pre8_padding=" << pad8
                  << " owner=" << target.resolved_owner << " before16=";
        if (have_before) {
            printBytes(before.data(), before.size());
        }
        else {
            std::cout << "unavailable";
        }
        std::cout << " entry16=";
        printBytes(target.original_bytes.data(), target.original_bytes.size());
        std::cout << '\n';
    }

    std::cout << "stable-entry alignment summary: logical=" << targets.size() << " unique=" << unique_entries.size()
              << " atomic8=" << aligned8 << " atomic16=" << aligned16 << " rel32_thunks=" << rel32_thunks
              << " pre5_padding=" << pre5_padding << " pre8_padding=" << pre8_padding
              << " dynamic_code_allowed=" << dynamic_code_allowed << '\n';
    return 0;
}
