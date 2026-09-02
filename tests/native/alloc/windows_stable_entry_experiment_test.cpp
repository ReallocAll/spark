#include "native/alloc/windows_stable_entry_experiment.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using spark::stable_entry_experiment::RendezvousProof;
using spark::stable_entry_experiment::State;
using spark::stable_entry_experiment::StateMachine;
using spark::stable_entry_experiment::TargetRecord;

namespace {

TargetRecord target()
{
    TargetRecord value;
    value.logical_name = "malloc";
    value.requested_module = "ucrtbase.dll";
    value.resolved_address = 0x1000;
    value.resolved_owner = "C:/Windows/System32/ucrtbase.dll";
    value.patch_length = 5;
    value.original_bytes[0] = 0x48;
    value.installed_bytes[0] = 0xE9;
    value.hook_range = {0x2000, 0x2100};
    value.pre_guard_range = {0x2000, 0x2010};
    value.transition_range = {0x2010, 0x2020};
    value.trampoline_range = {0x3000, 0x3100};
    value.trampoline_return_range = {0x30F0, 0x3120};
    value.patch_window = {0x1000, 0x1010};
    return value;
}

RendezvousProof completeProof()
{
    RendezvousProof proof;
    proof.enumeration_complete = true;
    proof.second_enumeration_same = true;
    proof.suspend_complete = true;
    proof.context_complete = true;
    proof.active_hook_calls_zero = true;
    proof.thread_creation_excluded = true;
    proof.instruction_pointers = {0x4000, 0x5000};
    return proof;
}

StateMachine preparedMachine()
{
    StateMachine machine;
    std::string error;
    assert(machine.resolve({target()}, true, error));
    assert(machine.prepare(error));
    assert(machine.establishOriginalOwnership(error));
    return machine;
}

void testEmptyResolveFailsClosed()
{
    StateMachine machine;
    std::string error;
    assert(!machine.resolve({}, true, error));
    assert(machine.state() == State::Unsafe);
    assert(!machine.shutdownBackend());
}

void testDynamicCodePolicyFailsClosed()
{
    StateMachine machine;
    std::string error;
    assert(!machine.resolve({target()}, false, error));
    assert(machine.state() == State::Unsafe);
    assert(error.find("ProcessDynamicCodePolicy") != std::string::npos);
}

void testMissingRelocationPlanFailsClosed()
{
    TargetRecord unresolved_patch = target();
    unresolved_patch.patch_length = 0;
    StateMachine machine;
    std::string error;
    assert(machine.resolve({unresolved_patch}, true, error));
    assert(!machine.prepare(error));
    assert(machine.state() == State::Unsafe);
}

void testThreadCreationRaceIsPromotionBlocker()
{
    StateMachine machine = preparedMachine();
    RendezvousProof proof = completeProof();
    proof.thread_creation_excluded = false;
    std::string error;
    assert(!machine.beginInstall(proof, error));
    assert(machine.state() == State::Unsafe);
    assert(error.find("thread-creation exclusion") != std::string::npos);
    assert(!machine.shutdownBackend());
}

void testRendezvousFailureInjection()
{
    const auto expect_failure = [](auto mutate, const char *needle) {
        StateMachine machine = preparedMachine();
        RendezvousProof proof = completeProof();
        mutate(proof);
        std::string error;
        assert(!machine.beginInstall(proof, error));
        assert(machine.state() == State::Unsafe);
        assert(error.find(needle) != std::string::npos);
    };

    expect_failure([](RendezvousProof &proof) { proof.enumeration_complete = false; }, "enumeration");
    expect_failure([](RendezvousProof &proof) { proof.second_enumeration_same = false; }, "thread set changed");
    expect_failure([](RendezvousProof &proof) { proof.suspend_complete = false; }, "SuspendThread");
    expect_failure([](RendezvousProof &proof) { proof.context_complete = false; }, "GetThreadContext");
}

void testEveryExecutableCorridorBlocksQuiescence()
{
    const std::vector<std::uintptr_t> blocked{0x1004, 0x2004, 0x2014, 0x3004, 0x3104};
    for (const std::uintptr_t rip : blocked) {
        StateMachine machine = preparedMachine();
        RendezvousProof proof = completeProof();
        proof.instruction_pointers = {rip};
        std::string error;
        assert(!machine.beginInstall(proof, error));
        assert(machine.state() == State::Unsafe);
        assert(error.find("live thread RIP") != std::string::npos);
    }
}

void testOwnershipMutationFailsClosed()
{
    StateMachine machine = preparedMachine();
    RendezvousProof proof = completeProof();
    std::string error;
    assert(machine.beginInstall(proof, error));
    assert(machine.completeInstall(error));
    assert(machine.beginDetach(error));
    assert(!machine.entriesRestored(false, error));
    assert(machine.state() == State::Unsafe);
    assert(error.find("third-party mutation") != std::string::npos);
    assert(!machine.shutdownBackend());
}

void testActiveCounterAloneIsNotQuiescenceProof()
{
    StateMachine machine = preparedMachine();
    RendezvousProof proof = completeProof();
    std::string error;
    assert(machine.beginInstall(proof, error));
    assert(machine.completeInstall(error));
    assert(machine.beginDetach(error));
    assert(machine.entriesRestored(true, error));
    assert(machine.beginDrain(error));

    proof.active_hook_calls_zero = true;
    proof.thread_creation_excluded = false;
    assert(!machine.proveQuiescence(proof, error));
    assert(machine.state() == State::Unsafe);
}

void testFullLifecycleOnlyDestroysAfterProof()
{
    StateMachine machine = preparedMachine();
    RendezvousProof proof = completeProof();
    std::string error;
    assert(machine.beginInstall(proof, error));
    assert(machine.state() == State::Installing);
    assert(machine.completeInstall(error));
    assert(machine.state() == State::Installed);
    assert(!machine.shutdownBackend());
    assert(machine.beginDetach(error));
    assert(machine.entriesRestored(true, error));
    assert(machine.beginDrain(error));
    assert(machine.proveQuiescence(proof, error));
    assert(machine.state() == State::Detached);
    assert(machine.shutdownBackend());
    assert(machine.completeDetach(error));
    assert(machine.destroy(error));
    assert(machine.state() == State::Destroyed);
    assert(machine.shutdownBackend());
}

}  // namespace

int main()
{
    testEmptyResolveFailsClosed();
    testDynamicCodePolicyFailsClosed();
    testMissingRelocationPlanFailsClosed();
    testThreadCreationRaceIsPromotionBlocker();
    testRendezvousFailureInjection();
    testEveryExecutableCorridorBlocksQuiescence();
    testOwnershipMutationFailsClosed();
    testActiveCounterAloneIsNotQuiescenceProof();
    testFullLifecycleOnlyDestroysAfterProof();
    std::cout << "stable entry state-machine safety tests passed\n";
    return 0;
}
