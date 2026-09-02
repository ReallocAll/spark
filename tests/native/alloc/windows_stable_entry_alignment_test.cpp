#include "native/alloc/windows_stable_entry_experiment.h"

#ifndef _WIN32
#error "windows_stable_entry_alignment_test.cpp is Windows-only"
#endif

#include <cassert>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using spark::stable_entry_experiment::TargetRecord;
using spark::stable_entry_experiment::resolveWindowsAllocatorTargets;

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
    std::set<std::uintptr_t> unique_entries;
    for (const TargetRecord &target : targets) {
        const bool a8 = (target.resolved_address & 7U) == 0;
        const bool a16 = (target.resolved_address & 15U) == 0;
        aligned8 += a8 ? 1U : 0U;
        aligned16 += a16 ? 1U : 0U;
        unique_entries.insert(target.resolved_address);
        std::cout << target.requested_module << '!' << target.logical_name << " entry=0x" << std::hex
                  << target.resolved_address << std::dec << " align8=" << a8 << " align16=" << a16
                  << " owner=" << target.resolved_owner << " bytes=";
        for (std::size_t i = 0; i < 16; ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(target.original_bytes[i]);
        }
        std::cout << std::dec << '\n';
    }

    std::cout << "stable-entry alignment summary: logical=" << targets.size() << " unique=" << unique_entries.size()
              << " atomic8=" << aligned8 << " atomic16=" << aligned16
              << " dynamic_code_allowed=" << dynamic_code_allowed << '\n';
    // Alignment is reported rather than assumed. Promotion logic may choose an
    // alternate resolved layer or a different atomic strategy for any target
    // that is not eligible for the preferred 8-byte transaction.
    return 0;
}
