#include "native/alloc/windows_iat_hooks.h"

#include <algorithm>
#include <functional>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace spark {
namespace {

struct SlotKey {
    WindowsIatModuleIdentity module;
    std::uintptr_t slot_rva = 0;

    friend bool operator==(const SlotKey &, const SlotKey &) = default;
};

struct SlotKeyHash {
    std::size_t operator()(const SlotKey &key) const noexcept
    {
        std::size_t value = std::hash<std::uintptr_t>{}(key.module.base);
        const auto mix = [&value](std::size_t part) {
            value ^= part + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        };
        mix(std::hash<std::uint32_t>{}(key.module.image_size));
        mix(std::hash<std::uint32_t>{}(key.module.timestamp));
        mix(std::hash<std::uint32_t>{}(key.module.checksum));
        mix(std::hash<std::uintptr_t>{}(key.slot_rva));
        return value;
    }
};

SlotKey slotKey(const WindowsIatSlot &slot) noexcept
{
    return {.module = slot.module, .slot_rva = slot.slot_rva};
}

bool validTarget(const WindowsIatHookTarget &target) noexcept
{
    return !target.import_name.empty() && target.original != nullptr && target.replacement != nullptr &&
           target.original != target.replacement;
}

}  // namespace

WindowsIatHooks::WindowsIatHooks(std::unique_ptr<WindowsIatHookBackend> backend) : backend_(std::move(backend)) {}

WindowsIatHooks::~WindowsIatHooks()
{
    try {
        std::string ignored;
        (void)uninstall(ignored);
    }
    catch (...) {
        // The owning AllocationSampler must prove detach before module unload.
        // The destructor is only a best-effort fallback and must not throw.
        return;
    }
}

bool WindowsIatHooks::configure(std::vector<WindowsIatHookTarget> targets, std::string &error)
{
    error.clear();
    if (backend_ == nullptr) {
        error = "Windows IAT hook backend is unavailable";
        return false;
    }
    if (installed_ || !active_slots_.empty()) {
        error = "Windows IAT hooks must be uninstalled before reconfiguration";
        return false;
    }
    if (unsafe_state_) {
        error = "Windows IAT hook state is unsafe after a previous lifecycle failure";
        return false;
    }
    if (targets.empty()) {
        error = "Windows IAT hook target list is empty";
        return false;
    }

    std::unordered_set<std::string> names;
    names.reserve(targets.size());
    for (const WindowsIatHookTarget &target : targets) {
        if (!validTarget(target)) {
            error = "Windows IAT hook target is invalid: " + target.import_name;
            return false;
        }
        if (!names.insert(target.import_name).second) {
            error = "Windows IAT hook target is duplicated: " + target.import_name;
            return false;
        }
    }

    targets_ = std::move(targets);
    return true;
}

bool WindowsIatHooks::install(std::string &error)
{
    error.clear();
    if (unsafe_state_) {
        error = "Windows IAT hook state is unsafe after a previous lifecycle failure";
        return false;
    }
    if (installed_) {
        return true;
    }
    if (targets_.empty()) {
        error = "Windows IAT hooks are not configured";
        return false;
    }

    if (reconcile(true, error) && requiredCoverageSatisfied(error) && !active_slots_.empty()) {
        installed_ = true;
        return true;
    }
    if (error.empty()) {
        error = "Windows IAT hook scan found no patchable allocator import slots";
    }

    const std::string install_error = error;
    std::string rollback_error;
    if (!rollbackInstalled(rollback_error)) {
        error = install_error + "; rollback failed: " + rollback_error;
        return false;
    }
    error = install_error;
    return false;
}

bool WindowsIatHooks::refresh(std::string &error)
{
    error.clear();
    if (unsafe_state_) {
        error = "Windows IAT hook state is unsafe after a previous lifecycle failure";
        return false;
    }
    if (targets_.empty()) {
        error = "Windows IAT hooks are not configured";
        return false;
    }

    for (auto it = active_slots_.begin(); it != active_slots_.end();) {
        void *value = nullptr;
        std::string read_error;
        const WindowsIatAccessStatus status = backend_->read(*it, value, read_error);
        if (status == WindowsIatAccessStatus::Stale ||
            (status == WindowsIatAccessStatus::Accessible && value != targets_[it->target_index].replacement)) {
            it = active_slots_.erase(it);
            continue;
        }
        if (status == WindowsIatAccessStatus::Error) {
            error = read_error.empty() ? "failed to revalidate an active Windows IAT slot" : read_error;
            return false;
        }
        ++it;
    }

    if (!reconcile(false, error)) {
        installed_ = !active_slots_.empty();
        return false;
    }
    installed_ = !active_slots_.empty();
    return true;
}

bool WindowsIatHooks::uninstall(std::string &error)
{
    error.clear();
    bool ok = true;
    std::string first_error;

    for (auto it = active_slots_.begin(); it != active_slots_.end();) {
        std::string slot_error;
        if (restoreSlot(*it, slot_error)) {
            it = active_slots_.erase(it);
            continue;
        }
        ok = false;
        if (first_error.empty()) {
            first_error = slot_error;
        }
        ++it;
    }

    installed_ = !active_slots_.empty();
    if (!ok) {
        error = first_error.empty() ? "failed to detach one or more Windows IAT hooks" : first_error;
        return false;
    }
    if (active_slots_.empty()) {
        unsafe_state_ = false;
    }
    return true;
}

bool WindowsIatHooks::installed() const noexcept
{
    return installed_;
}

bool WindowsIatHooks::unsafeState() const noexcept
{
    return unsafe_state_;
}

std::size_t WindowsIatHooks::activeSlotCount() const noexcept
{
    return active_slots_.size();
}

const std::vector<WindowsIatSlot> &WindowsIatHooks::activeSlots() const noexcept
{
    return active_slots_;
}

const std::vector<WindowsIatHookTarget> &WindowsIatHooks::targets() const noexcept
{
    return targets_;
}

bool WindowsIatHooks::reconcile(bool initial_install, std::string &error)
{
    std::vector<WindowsIatSlot> discovered;
    if (!backend_->enumerate(targets_, discovered, error)) {
        return false;
    }

    std::unordered_set<SlotKey, SlotKeyHash> seen;
    seen.reserve(discovered.size());
    for (const WindowsIatSlot &slot : discovered) {
        if (slot.target_index >= targets_.size()) {
            error = "Windows IAT backend returned an invalid target index";
            return false;
        }
        if (!seen.insert(slotKey(slot)).second || slotAlreadyTracked(slot)) {
            continue;
        }

        const WindowsIatHookTarget &target = targets_[slot.target_index];
        void *current = nullptr;
        std::string read_error;
        const WindowsIatAccessStatus read_status = backend_->read(slot, current, read_error);
        if (read_status == WindowsIatAccessStatus::Stale) {
            continue;
        }
        if (read_status == WindowsIatAccessStatus::Error) {
            error = read_error.empty() ? "failed to read a Windows IAT slot" : read_error;
            return false;
        }
        if (current == target.replacement) {
            error = initial_install ? "Windows IAT slot already points at this hook before ownership was established"
                                    : "untracked Windows IAT slot points at this hook";
            return false;
        }
        if (current != target.original) {
            continue;  // Another interceptor owns this slot. Never overwrite it.
        }

        std::string exchange_error;
        const WindowsIatExchangeResult exchange =
            backend_->compareExchange(slot, target.original, target.replacement, exchange_error);
        if (exchange.status == WindowsIatExchangeStatus::Exchanged) {
            active_slots_.push_back(slot);
            continue;
        }
        if (exchange.status == WindowsIatExchangeStatus::Mismatch ||
            exchange.status == WindowsIatExchangeStatus::Stale) {
            continue;
        }

        // An OS failure can happen after the pointer exchange (for example while
        // restoring page protection). Re-read before deciding whether rollback is required.
        current = nullptr;
        read_error.clear();
        const WindowsIatAccessStatus after_status = backend_->read(slot, current, read_error);
        if (after_status == WindowsIatAccessStatus::Accessible && current == target.replacement) {
            active_slots_.push_back(slot);
            error = exchange_error.empty() ? "Windows IAT exchange failed after installing the hook" : exchange_error;
            return false;
        }
        if (after_status == WindowsIatAccessStatus::Error) {
            // Ownership is uncertain. Keep the slot as potentially active so
            // shutdown cannot silently forget a pointer that may target Spark.
            active_slots_.push_back(slot);
            installed_ = true;
            markUnsafe(exchange_error.empty() ? read_error : exchange_error, error);
            return false;
        }
        error = exchange_error.empty() ? "Windows IAT exchange failed" : exchange_error;
        return false;
    }
    return true;
}

bool WindowsIatHooks::rollbackInstalled(std::string &error)
{
    bool ok = true;
    std::string first_error;
    for (auto it = active_slots_.begin(); it != active_slots_.end();) {
        std::string slot_error;
        if (restoreSlot(*it, slot_error)) {
            it = active_slots_.erase(it);
            continue;
        }
        ok = false;
        if (first_error.empty()) {
            first_error = slot_error;
        }
        ++it;
    }
    installed_ = !active_slots_.empty();
    if (!ok) {
        error = first_error.empty() ? "Windows IAT rollback left a hook active" : first_error;
    }
    return ok;
}

bool WindowsIatHooks::restoreSlot(const WindowsIatSlot &slot, std::string &error)
{
    if (slot.target_index >= targets_.size()) {
        markUnsafe("active Windows IAT slot has an invalid target index", error);
        return false;
    }

    const WindowsIatHookTarget &target = targets_[slot.target_index];
    void *current = nullptr;
    std::string read_error;
    const WindowsIatAccessStatus read_status = backend_->read(slot, current, read_error);
    if (read_status == WindowsIatAccessStatus::Stale) {
        return true;
    }
    if (read_status == WindowsIatAccessStatus::Error) {
        markUnsafe(read_error.empty() ? "failed to revalidate a Windows IAT slot during detach" : read_error, error);
        return false;
    }
    if (current != target.replacement) {
        return true;  // Already detached or replaced by another interceptor; do not clobber it.
    }

    std::string exchange_error;
    const WindowsIatExchangeResult exchange =
        backend_->compareExchange(slot, target.replacement, target.original, exchange_error);
    if (exchange.status == WindowsIatExchangeStatus::Exchanged || exchange.status == WindowsIatExchangeStatus::Stale) {
        return true;
    }
    if (exchange.status == WindowsIatExchangeStatus::Mismatch && exchange.observed != target.replacement) {
        return true;
    }

    current = nullptr;
    read_error.clear();
    const WindowsIatAccessStatus after_status = backend_->read(slot, current, read_error);
    if (after_status == WindowsIatAccessStatus::Stale ||
        (after_status == WindowsIatAccessStatus::Accessible && current != target.replacement)) {
        return true;
    }
    std::string reason = "Windows IAT slot still points at the hook";
    if (!read_error.empty()) {
        reason = read_error;
    }
    if (!exchange_error.empty()) {
        reason = exchange_error;
    }
    markUnsafe(reason, error);
    return false;
}

bool WindowsIatHooks::slotAlreadyTracked(const WindowsIatSlot &slot) const noexcept
{
    return std::ranges::any_of(active_slots_, [&slot](const WindowsIatSlot &active) {
        return active.module == slot.module && active.slot_rva == slot.slot_rva;
    });
}

bool WindowsIatHooks::requiredCoverageSatisfied(std::string &error) const
{
    for (std::size_t target_index = 0; target_index < targets_.size(); ++target_index) {
        if (!targets_[target_index].required) {
            continue;
        }
        const bool covered = std::ranges::any_of(
            active_slots_, [target_index](const WindowsIatSlot &slot) { return slot.target_index == target_index; });
        if (!covered) {
            error = "required Windows IAT hook has no owned import slot: " + targets_[target_index].import_name;
            return false;
        }
    }
    return true;
}

void WindowsIatHooks::markUnsafe(const std::string &reason, std::string &error) noexcept
{
    unsafe_state_ = true;
    try {
        error = reason.empty() ? "Windows IAT hook state cannot be proven safe" : reason;
    }
    catch (...) {
        error.clear();
    }
}

}  // namespace spark
