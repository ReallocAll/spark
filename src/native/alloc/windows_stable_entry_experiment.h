#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spark::stable_entry_experiment {

enum class State {
    Empty,
    Resolved,
    Prepared,
    OwnedOriginal,
    Installing,
    Installed,
    Detaching,
    EntriesRestored,
    Draining,
    QuiescenceProof,
    Detached,
    Destroyed,
    Unsafe,
};

struct AddressRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;

    [[nodiscard]] bool contains(std::uintptr_t address) const noexcept
    {
        return begin < end && address >= begin && address < end;
    }
};

struct TargetRecord {
    std::string logical_name;
    std::string requested_module;
    std::uintptr_t resolved_address = 0;
    std::string resolved_owner;
    std::array<std::uint8_t, 16> original_bytes{};
    std::array<std::uint8_t, 16> installed_bytes{};
    std::size_t patch_length = 0;
    std::uint64_t original_hash = 0;
    std::uint64_t installed_hash = 0;
    AddressRange hook_range{};
    AddressRange pre_guard_range{};
    AddressRange transition_range{};
    AddressRange trampoline_range{};
    AddressRange trampoline_return_range{};
    AddressRange patch_window{};
};

struct RendezvousProof {
    bool enumeration_complete = false;
    bool second_enumeration_same = false;
    bool suspend_complete = false;
    bool context_complete = false;
    bool active_hook_calls_zero = false;
    bool thread_creation_excluded = false;
    std::vector<std::uintptr_t> instruction_pointers;
};

class StateMachine {
public:
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool dynamicCodeAllowed() const noexcept { return dynamic_code_allowed_; }
    [[nodiscard]] bool shutdownBackend() const noexcept
    {
        return state_ == State::Detached || state_ == State::Destroyed;
    }
    [[nodiscard]] const std::string &failureReason() const noexcept { return failure_reason_; }
    [[nodiscard]] const std::vector<TargetRecord> &targets() const noexcept { return targets_; }

    bool resolve(std::vector<TargetRecord> targets, bool dynamic_code_allowed, std::string &error);
    bool prepare(std::string &error);
    bool establishOriginalOwnership(std::string &error);
    bool beginInstall(const RendezvousProof &proof, std::string &error);
    bool completeInstall(std::string &error);
    bool beginDetach(std::string &error);
    bool entriesRestored(bool ownership_preserved, std::string &error);
    bool beginDrain(std::string &error);
    bool proveQuiescence(const RendezvousProof &proof, std::string &error);
    bool completeDetach(std::string &error);
    bool destroy(std::string &error);
    void markUnsafe(std::string reason, std::string &error) noexcept;

    [[nodiscard]] bool ripTouchesSparkExecutable(const RendezvousProof &proof) const noexcept;

private:
    bool requireState(State expected, const char *operation, std::string &error);
    bool validateRendezvous(const RendezvousProof &proof, const char *phase, std::string &error);

    State state_ = State::Empty;
    bool dynamic_code_allowed_ = false;
    std::string failure_reason_;
    std::vector<TargetRecord> targets_;
};

std::uint64_t hashBytes(const std::uint8_t *data, std::size_t size) noexcept;

#ifdef _WIN32
bool resolveWindowsAllocatorTargets(std::vector<TargetRecord> &targets, bool &dynamic_code_allowed, std::string &error);
#endif

}  // namespace spark::stable_entry_experiment
