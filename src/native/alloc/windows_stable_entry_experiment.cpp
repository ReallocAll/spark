#include "native/alloc/windows_stable_entry_experiment.h"

#include <utility>

namespace spark::stable_entry_experiment {
namespace {

bool validResolvedTarget(const TargetRecord &target) noexcept
{
    return !target.logical_name.empty() && !target.requested_module.empty() && target.resolved_address != 0 &&
           !target.resolved_owner.empty();
}

}  // namespace

std::uint64_t hashBytes(const std::uint8_t *data, std::size_t size) noexcept
{
    std::uint64_t value = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        value ^= data[index];
        value *= 1099511628211ULL;
    }
    return value;
}

bool StateMachine::requireState(State expected, const char *operation, std::string &error)
{
    if (state_ == State::Unsafe) {
        error = failure_reason_.empty() ? "stable entry backend is unsafe" : failure_reason_;
        return false;
    }
    if (state_ != expected) {
        error = std::string(operation) + " is invalid in the current stable entry state";
        return false;
    }
    return true;
}

void StateMachine::markUnsafe(std::string reason, std::string &error) noexcept
{
    state_ = State::Unsafe;
    try {
        failure_reason_ = reason.empty() ? "stable entry safety cannot be proven" : std::move(reason);
        error = failure_reason_;
    }
    catch (...) {
        failure_reason_.clear();
        error.clear();
    }
}

bool StateMachine::resolve(std::vector<TargetRecord> targets, bool dynamic_code_allowed, std::string &error)
{
    error.clear();
    if (!requireState(State::Empty, "resolve", error)) {
        return false;
    }
    if (targets.empty()) {
        markUnsafe("stable entry resolver returned no allocator targets", error);
        return false;
    }
    for (const TargetRecord &target : targets) {
        if (!validResolvedTarget(target)) {
            markUnsafe("stable entry resolver returned incomplete target provenance", error);
            return false;
        }
    }
    targets_ = std::move(targets);
    dynamic_code_allowed_ = dynamic_code_allowed;
    if (!dynamic_code_allowed_) {
        markUnsafe("ProcessDynamicCodePolicy prohibits executable-code modification", error);
        return false;
    }
    state_ = State::Resolved;
    return true;
}

bool StateMachine::prepare(std::string &error)
{
    error.clear();
    if (!requireState(State::Resolved, "prepare", error)) {
        return false;
    }
    for (TargetRecord &target : targets_) {
        if (target.patch_length == 0 || target.patch_length > target.original_bytes.size()) {
            markUnsafe("stable entry target has no bounded patch plan", error);
            return false;
        }
        target.original_hash = hashBytes(target.original_bytes.data(), target.patch_length);
    }
    state_ = State::Prepared;
    return true;
}

bool StateMachine::establishOriginalOwnership(std::string &error)
{
    error.clear();
    if (!requireState(State::Prepared, "establishOriginalOwnership", error)) {
        return false;
    }
    state_ = State::OwnedOriginal;
    return true;
}

bool StateMachine::ripTouchesSparkExecutable(const RendezvousProof &proof) const noexcept
{
    for (const std::uintptr_t rip : proof.instruction_pointers) {
        for (const TargetRecord &target : targets_) {
            if (target.hook_range.contains(rip) || target.pre_guard_range.contains(rip) ||
                target.transition_range.contains(rip) || target.trampoline_range.contains(rip) ||
                target.trampoline_return_range.contains(rip) || target.patch_window.contains(rip)) {
                return true;
            }
        }
    }
    return false;
}

bool StateMachine::validateRendezvous(const RendezvousProof &proof, const char *phase, std::string &error)
{
    if (!proof.enumeration_complete) {
        markUnsafe(std::string(phase) + ": thread enumeration failed", error);
        return false;
    }
    if (!proof.second_enumeration_same) {
        markUnsafe(std::string(phase) + ": thread set changed during rendezvous", error);
        return false;
    }
    if (!proof.suspend_complete) {
        markUnsafe(std::string(phase) + ": SuspendThread failed for at least one live thread", error);
        return false;
    }
    if (!proof.context_complete) {
        markUnsafe(std::string(phase) + ": GetThreadContext failed for at least one live thread", error);
        return false;
    }
    if (!proof.thread_creation_excluded) {
        markUnsafe(std::string(phase) + ": no process-wide thread-creation exclusion exists; a thread may appear after "
                                        "enumeration and enter the patch window",
                   error);
        return false;
    }
    if (ripTouchesSparkExecutable(proof)) {
        markUnsafe(std::string(phase) + ": a live thread RIP is inside a Spark-owned hook/transition/trampoline range",
                   error);
        return false;
    }
    return true;
}

bool StateMachine::beginInstall(const RendezvousProof &proof, std::string &error)
{
    error.clear();
    if (!requireState(State::OwnedOriginal, "beginInstall", error)) {
        return false;
    }
    if (!validateRendezvous(proof, "install", error)) {
        return false;
    }
    state_ = State::Installing;
    return true;
}

bool StateMachine::completeInstall(std::string &error)
{
    error.clear();
    if (!requireState(State::Installing, "completeInstall", error)) {
        return false;
    }
    for (TargetRecord &target : targets_) {
        target.installed_hash = hashBytes(target.installed_bytes.data(), target.patch_length);
    }
    state_ = State::Installed;
    return true;
}

bool StateMachine::beginDetach(std::string &error)
{
    error.clear();
    if (!requireState(State::Installed, "beginDetach", error)) {
        return false;
    }
    state_ = State::Detaching;
    return true;
}

bool StateMachine::entriesRestored(bool ownership_preserved, std::string &error)
{
    error.clear();
    if (!requireState(State::Detaching, "entriesRestored", error)) {
        return false;
    }
    if (!ownership_preserved) {
        markUnsafe("stable entry restore ownership was lost to a third-party mutation", error);
        return false;
    }
    state_ = State::EntriesRestored;
    return true;
}

bool StateMachine::beginDrain(std::string &error)
{
    error.clear();
    if (!requireState(State::EntriesRestored, "beginDrain", error)) {
        return false;
    }
    state_ = State::Draining;
    return true;
}

bool StateMachine::proveQuiescence(const RendezvousProof &proof, std::string &error)
{
    error.clear();
    if (!requireState(State::Draining, "proveQuiescence", error)) {
        return false;
    }
    state_ = State::QuiescenceProof;
    if (!proof.active_hook_calls_zero) {
        markUnsafe("detach: active_hook_calls is non-zero", error);
        return false;
    }
    if (!validateRendezvous(proof, "detach", error)) {
        return false;
    }
    state_ = State::Detached;
    return true;
}

bool StateMachine::completeDetach(std::string &error)
{
    error.clear();
    if (!requireState(State::Detached, "completeDetach", error)) {
        return false;
    }
    return true;
}

bool StateMachine::destroy(std::string &error)
{
    error.clear();
    if (!requireState(State::Detached, "destroy", error)) {
        return false;
    }
    state_ = State::Destroyed;
    return true;
}

}  // namespace spark::stable_entry_experiment
