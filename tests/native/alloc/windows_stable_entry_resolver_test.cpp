#include "native/alloc/windows_stable_entry_experiment.h"

#ifndef _WIN32
#error "windows_stable_entry_resolver_test.cpp is Windows-only"
#endif

#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using spark::stable_entry_experiment::State;
using spark::stable_entry_experiment::StateMachine;
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

    std::set<std::string> names;
    for (const TargetRecord &target : targets) {
        assert(target.resolved_address != 0);
        assert(!target.requested_module.empty());
        assert(!target.resolved_owner.empty());
        assert(target.original_hash != 0);
        assert(target.patch_window.begin == target.resolved_address);
        assert(target.patch_window.end > target.patch_window.begin);
        assert(names.insert(target.logical_name).second);
        std::cout << target.requested_module << '!' << target.logical_name << " -> 0x" << std::hex
                  << target.resolved_address << std::dec << " owner=" << target.resolved_owner << '\n';
    }

    StateMachine machine;
    if (!dynamic_code_allowed) {
        assert(!machine.resolve(targets, false, error));
        assert(machine.state() == State::Unsafe);
        assert(error.find("ProcessDynamicCodePolicy") != std::string::npos);
        std::cout << "stable entry unavailable: dynamic code policy prohibits executable patching\n";
        return 0;
    }

    assert(machine.resolve(targets, true, error));
    assert(machine.state() == State::Resolved);

    // Resolution intentionally does not guess an instruction relocation length.
    // Until a relocation engine proves a bounded patch plan for every resolved
    // entry, the experiment must stop in Unsafe rather than patch executable code.
    assert(!machine.prepare(error));
    assert(machine.state() == State::Unsafe);
    assert(error.find("bounded patch plan") != std::string::npos);
    assert(!machine.shutdownBackend());
    std::cout << "stable entry native probe stopped fail-closed before executable patching: " << error << '\n';
    return 0;
}
