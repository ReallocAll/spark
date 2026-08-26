#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "native/alloc/windows_iat_hooks.h"

namespace {

constexpr std::uintptr_t KOriginalAddress = 0x1000;
constexpr std::uintptr_t KReplacementAddress = 0x2000;
constexpr std::uintptr_t KForeignAddress = 0x3000;

void *pointer(std::uintptr_t value) noexcept
{
    // Synthetic pointers are never dereferenced by the fake backend.
    return reinterpret_cast<void *>(value);  // NOLINT(performance-no-int-to-ptr)
}

spark::WindowsIatModuleIdentity module(std::uintptr_t base, std::uint32_t timestamp)
{
    return {.base = base, .image_size = 0x4000, .timestamp = timestamp, .checksum = timestamp ^ 0x55AAU};
}

spark::WindowsIatHookTarget target(bool required = true)
{
    return {.import_name = "malloc",
            .original = pointer(KOriginalAddress),
            .replacement = pointer(KReplacementAddress),
            .required = required};
}

class FakeWindowsIatBackend final : public spark::WindowsIatHookBackend {
public:
    struct State {
        spark::WindowsIatSlot slot;
        void *value = nullptr;
        bool advertised = true;
        bool stale = false;
        bool read_error = false;
        bool exchange_error_before = false;
        bool exchange_error_after = false;
        bool poison_reads_after_exchange = false;
        std::size_t reads = 0;
        std::size_t exchanges = 0;
    };

    std::size_t add(const spark::WindowsIatModuleIdentity &identity, std::uintptr_t rva, std::size_t target_index,
                    void *value)
    {
        states_.push_back(
            {.slot = {.module = identity, .slot_rva = rva, .target_index = target_index}, .value = value});
        return states_.size() - 1;
    }

    State &state(std::size_t index) { return states_.at(index); }
    const State &state(std::size_t index) const { return states_.at(index); }

    void failEnumeration(bool fail) noexcept { fail_enumeration_ = fail; }

    bool enumerate(const std::vector<spark::WindowsIatHookTarget> &, std::vector<spark::WindowsIatSlot> &slots,
                   std::string &error) override
    {
        ++enumerations_;
        if (fail_enumeration_) {
            error = "injected enumeration failure";
            return false;
        }
        slots.clear();
        for (const State &entry : states_) {
            if (entry.advertised) {
                slots.push_back(entry.slot);
            }
        }
        return true;
    }

    spark::WindowsIatAccessStatus read(const spark::WindowsIatSlot &slot, void *&value,
                                       std::string &error) noexcept override
    {
        State *entry = find(slot);
        if (entry == nullptr || entry->stale) {
            return spark::WindowsIatAccessStatus::Stale;
        }
        ++entry->reads;
        if (entry->read_error) {
            try {
                error = "injected read failure";
            }
            catch (...) {
                error.clear();
            }
            return spark::WindowsIatAccessStatus::Error;
        }
        value = entry->value;
        return spark::WindowsIatAccessStatus::Accessible;
    }

    spark::WindowsIatExchangeResult compareExchange(const spark::WindowsIatSlot &slot, void *expected, void *desired,
                                                    std::string &error) noexcept override
    {
        State *entry = find(slot);
        if (entry == nullptr || entry->stale) {
            return {.status = spark::WindowsIatExchangeStatus::Stale, .observed = nullptr};
        }
        ++entry->exchanges;
        if (entry->exchange_error_before) {
            try {
                error = "injected exchange failure before write";
            }
            catch (...) {
                error.clear();
            }
            return {.status = spark::WindowsIatExchangeStatus::Error, .observed = entry->value};
        }
        if (entry->value != expected) {
            return {.status = spark::WindowsIatExchangeStatus::Mismatch, .observed = entry->value};
        }

        void *observed = entry->value;
        entry->value = desired;
        if (entry->exchange_error_after) {
            if (entry->poison_reads_after_exchange) {
                entry->read_error = true;
            }
            try {
                error = "injected exchange failure after write";
            }
            catch (...) {
                error.clear();
            }
            return {.status = spark::WindowsIatExchangeStatus::Error, .observed = observed};
        }
        return {.status = spark::WindowsIatExchangeStatus::Exchanged, .observed = observed};
    }

    [[nodiscard]] std::size_t enumerations() const noexcept { return enumerations_; }

private:
    State *find(const spark::WindowsIatSlot &slot) noexcept
    {
        for (State &entry : states_) {
            if (entry.slot.module == slot.module && entry.slot.slot_rva == slot.slot_rva) {
                return &entry;
            }
        }
        return nullptr;
    }

    std::vector<State> states_;
    bool fail_enumeration_ = false;
    std::size_t enumerations_ = 0;
};

struct Fixture {
    std::unique_ptr<FakeWindowsIatBackend> backend = std::make_unique<FakeWindowsIatBackend>();
    FakeWindowsIatBackend *raw = backend.get();
    spark::WindowsIatHooks hooks{std::move(backend)};
    std::string error;

    bool configure(bool required = true) { return hooks.configure({target(required)}, error); }
};

bool require(bool condition, const char *message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "windows IAT hooks: %s\n", message);
    return false;
}

bool testConfigureValidation()
{
    Fixture fixture;
    if (!require(!fixture.hooks.configure({}, fixture.error), "empty target list was accepted")) {
        return false;
    }

    auto invalid = target();
    invalid.replacement = invalid.original;
    if (!require(!fixture.hooks.configure({invalid}, fixture.error), "self-replacing target was accepted")) {
        return false;
    }

    auto duplicate = target();
    return require(!fixture.hooks.configure({target(), duplicate}, fixture.error), "duplicate target was accepted");
}

bool testInstallUninstallAndIdempotence()
{
    Fixture fixture;
    const std::size_t slot = fixture.raw->add(module(0x100000, 1), 0x280, 0, pointer(KOriginalAddress));
    if (!require(fixture.configure(), "configure failed") ||
        !require(fixture.hooks.install(fixture.error), "install failed") ||
        !require(fixture.hooks.installed() && fixture.hooks.activeSlotCount() == 1,
                 "install state was not published") ||
        !require(fixture.raw->state(slot).value == pointer(KReplacementAddress), "slot was not patched")) {
        return false;
    }

    const std::size_t exchanges = fixture.raw->state(slot).exchanges;
    if (!require(fixture.hooks.install(fixture.error), "idempotent install failed") ||
        !require(fixture.raw->state(slot).exchanges == exchanges, "idempotent install patched twice")) {
        return false;
    }

    return require(fixture.hooks.uninstall(fixture.error), "uninstall failed") &&
           require(!fixture.hooks.installed() && fixture.hooks.activeSlotCount() == 0,
                   "uninstall retained ownership") &&
           require(fixture.raw->state(slot).value == pointer(KOriginalAddress), "uninstall did not restore slot") &&
           require(fixture.hooks.uninstall(fixture.error), "idempotent uninstall failed");
}

bool testPartialFailureRollsBack()
{
    Fixture fixture;
    const auto identity = module(0x200000, 2);
    const std::size_t first = fixture.raw->add(identity, 0x300, 0, pointer(KOriginalAddress));
    const std::size_t second = fixture.raw->add(identity, 0x308, 0, pointer(KOriginalAddress));
    fixture.raw->state(second).exchange_error_before = true;

    return require(fixture.configure(), "configure failed for rollback test") &&
           require(!fixture.hooks.install(fixture.error), "partial install failure was reported as success") &&
           require(fixture.raw->state(first).value == pointer(KOriginalAddress), "first slot was not rolled back") &&
           require(fixture.raw->state(second).value == pointer(KOriginalAddress), "failed slot was modified") &&
           require(!fixture.hooks.installed() && fixture.hooks.activeSlotCount() == 0,
                   "rollback retained active slots") &&
           require(!fixture.hooks.unsafeState(), "recoverable rollback was marked permanently unsafe");
}

bool testPostWriteFailureRollsBackKnownOwnership()
{
    Fixture fixture;
    const std::size_t slot = fixture.raw->add(module(0x300000, 3), 0x320, 0, pointer(KOriginalAddress));
    fixture.raw->state(slot).exchange_error_after = true;

    return require(fixture.configure(), "configure failed for post-write rollback") &&
           require(!fixture.hooks.install(fixture.error), "post-write failure was reported as success") &&
           require(fixture.raw->state(slot).value == pointer(KOriginalAddress),
                   "known post-write hook was not rolled back") &&
           require(fixture.hooks.activeSlotCount() == 0 && !fixture.hooks.unsafeState(),
                   "known post-write rollback retained unsafe state");
}

bool testForeignHookIsNeverOverwritten()
{
    Fixture fixture;
    const auto identity = module(0x400000, 4);
    const std::size_t foreign = fixture.raw->add(identity, 0x340, 0, pointer(KForeignAddress));
    const std::size_t owned = fixture.raw->add(identity, 0x348, 0, pointer(KOriginalAddress));

    if (!require(fixture.configure(), "configure failed for foreign-hook test") ||
        !require(fixture.hooks.install(fixture.error), "install failed with one patchable slot")) {
        return false;
    }
    if (!require(fixture.raw->state(foreign).value == pointer(KForeignAddress) &&
                     fixture.raw->state(foreign).exchanges == 0,
                 "foreign hook was overwritten") ||
        !require(fixture.raw->state(owned).value == pointer(KReplacementAddress), "owned slot was not patched")) {
        return false;
    }

    // A third party taking ownership after us also wins: detach must never write
    // the old original over a pointer that no longer belongs to Spark.
    fixture.raw->state(owned).value = pointer(KForeignAddress);
    return require(fixture.hooks.uninstall(fixture.error), "detach after foreign takeover failed") &&
           require(fixture.raw->state(owned).value == pointer(KForeignAddress), "detach clobbered foreign takeover") &&
           require(!fixture.hooks.installed(), "foreign takeover was retained as an owned hook");
}

bool testRequiredTargetWithoutOwnershipFailsClosed()
{
    Fixture fixture;
    const std::size_t foreign = fixture.raw->add(module(0x500000, 5), 0x360, 0, pointer(KForeignAddress));

    return require(fixture.configure(), "configure failed for required coverage test") &&
           require(!fixture.hooks.install(fixture.error), "required target without ownership succeeded") &&
           require(fixture.raw->state(foreign).value == pointer(KForeignAddress) &&
                       fixture.raw->state(foreign).exchanges == 0,
                   "required coverage failure modified foreign hook") &&
           require(!fixture.hooks.installed() && !fixture.hooks.unsafeState(),
                   "coverage-only failure left unsafe state");
}

bool testModuleUnloadAndAddressReuse()
{
    Fixture fixture;
    const auto first_identity = module(0x600000, 6);
    const std::size_t first = fixture.raw->add(first_identity, 0x380, 0, pointer(KOriginalAddress));
    if (!require(fixture.configure(), "configure failed for address-reuse test") ||
        !require(fixture.hooks.install(fixture.error), "initial install failed for address-reuse test")) {
        return false;
    }

    fixture.raw->state(first).stale = true;
    const auto reused_identity = module(first_identity.base, 7);
    const std::size_t reused = fixture.raw->add(reused_identity, 0x380, 0, pointer(KOriginalAddress));
    if (!require(fixture.hooks.refresh(fixture.error), "refresh failed after module address reuse") ||
        !require(fixture.raw->state(first).exchanges == 1, "stale module was written during refresh") ||
        !require(fixture.raw->state(reused).value == pointer(KReplacementAddress), "reused address was not patched") ||
        !require(fixture.hooks.activeSlotCount() == 1 && fixture.hooks.activeSlots().front().module == reused_identity,
                 "active registry retained stale module identity")) {
        return false;
    }

    return require(fixture.hooks.uninstall(fixture.error), "uninstall after address reuse failed") &&
           require(fixture.raw->state(reused).value == pointer(KOriginalAddress),
                   "reused module slot was not restored");
}

bool testRestoreFailureRetainsOwnershipUntilRetry()
{
    Fixture fixture;
    const std::size_t slot = fixture.raw->add(module(0x700000, 8), 0x3A0, 0, pointer(KOriginalAddress));
    if (!require(fixture.configure(), "configure failed for restore failure test") ||
        !require(fixture.hooks.install(fixture.error), "install failed for restore failure test")) {
        return false;
    }

    fixture.raw->state(slot).exchange_error_before = true;
    if (!require(!fixture.hooks.uninstall(fixture.error), "restore failure was reported as detached") ||
        !require(fixture.hooks.installed() && fixture.hooks.activeSlotCount() == 1 && fixture.hooks.unsafeState(),
                 "restore failure forgot active ownership") ||
        !require(fixture.raw->state(slot).value == pointer(KReplacementAddress),
                 "restore failure unexpectedly changed slot")) {
        return false;
    }

    fixture.raw->state(slot).exchange_error_before = false;
    return require(fixture.hooks.uninstall(fixture.error), "detach retry did not recover") &&
           require(fixture.raw->state(slot).value == pointer(KOriginalAddress),
                   "detach retry did not restore original") &&
           require(!fixture.hooks.installed() && !fixture.hooks.unsafeState(),
                   "successful retry did not clear unsafe state");
}

bool testUnknownPostExchangeStateCannotBeForgotten()
{
    Fixture fixture;
    const std::size_t slot = fixture.raw->add(module(0x800000, 9), 0x3C0, 0, pointer(KOriginalAddress));
    fixture.raw->state(slot).exchange_error_after = true;
    fixture.raw->state(slot).poison_reads_after_exchange = true;

    if (!require(fixture.configure(), "configure failed for unknown-state test") ||
        !require(!fixture.hooks.install(fixture.error), "unknown post-exchange state was reported as success") ||
        !require(fixture.hooks.installed() && fixture.hooks.activeSlotCount() == 1 && fixture.hooks.unsafeState(),
                 "unknown post-exchange state was forgotten")) {
        return false;
    }

    fixture.raw->state(slot).read_error = false;
    fixture.raw->state(slot).exchange_error_after = false;
    fixture.raw->state(slot).poison_reads_after_exchange = false;
    return require(fixture.hooks.uninstall(fixture.error), "unknown-state recovery detach failed") &&
           require(fixture.raw->state(slot).value == pointer(KOriginalAddress),
                   "unknown-state recovery did not restore slot") &&
           require(!fixture.hooks.installed() && !fixture.hooks.unsafeState(),
                   "unknown-state recovery did not clear state");
}

bool testEnumerationFailureDoesNotDisturbInstalledHooks()
{
    Fixture fixture;
    const std::size_t slot = fixture.raw->add(module(0x900000, 10), 0x3E0, 0, pointer(KOriginalAddress));
    if (!require(fixture.configure(), "configure failed for enumeration test") ||
        !require(fixture.hooks.install(fixture.error), "install failed for enumeration test")) {
        return false;
    }

    fixture.raw->failEnumeration(true);
    if (!require(!fixture.hooks.refresh(fixture.error), "enumeration failure was ignored") ||
        !require(fixture.hooks.installed() && fixture.hooks.activeSlotCount() == 1,
                 "enumeration failure discarded existing ownership") ||
        !require(fixture.raw->state(slot).value == pointer(KReplacementAddress),
                 "enumeration failure modified active slot")) {
        return false;
    }

    fixture.raw->failEnumeration(false);
    return require(fixture.hooks.uninstall(fixture.error), "cleanup after enumeration failure failed");
}

}  // namespace

int main()
{
    if (!testConfigureValidation() || !testInstallUninstallAndIdempotence() || !testPartialFailureRollsBack() ||
        !testPostWriteFailureRollsBackKnownOwnership() || !testForeignHookIsNeverOverwritten() ||
        !testRequiredTargetWithoutOwnershipFailsClosed() || !testModuleUnloadAndAddressReuse() ||
        !testRestoreFailureRetainsOwnershipUntilRetry() || !testUnknownPostExchangeStateCannotBeForgotten() ||
        !testEnumerationFailureDoesNotDisturbInstalledHooks()) {
        return 1;
    }
    return 0;
}
