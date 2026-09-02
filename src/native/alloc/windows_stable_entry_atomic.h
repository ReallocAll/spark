#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Keep Win32 declarations in the global namespace. The implementation includes
// these headers again after opening the experiment namespace; the SDK/header
// guards deliberately make those later includes no-ops.
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <funchook.h>
#endif

#include "native/alloc/windows_stable_entry_experiment.h"

namespace spark::stable_entry_experiment {

constexpr std::size_t kAtomicEntryWidth8 = 8;
constexpr std::size_t kAtomicEntryWidth16 = 16;
constexpr std::size_t kAbsoluteIndirectJumpSize = 14;

[[nodiscard]] bool isAlignedForAtomic8(std::uintptr_t address) noexcept;
[[nodiscard]] bool isAlignedForAtomic16(std::uintptr_t address) noexcept;
[[nodiscard]] bool rel32Reachable(std::uintptr_t instruction_end, std::uintptr_t destination) noexcept;

bool encodeAtomic8RelayEntry(std::uintptr_t entry, std::uintptr_t relay,
                             const std::array<std::uint8_t, 16> &original,
                             std::array<std::uint8_t, 16> &installed, std::string &error);
bool encodeAtomic16AbsoluteEntry(std::uintptr_t entry, std::uintptr_t hook,
                                 const std::array<std::uint8_t, 16> &original,
                                 std::array<std::uint8_t, 16> &installed, std::string &error);

#ifdef _WIN32

struct AtomicCompareResult {
    bool exchanged = false;
    std::array<std::uint8_t, 16> observed{};
};

[[nodiscard]] bool cpuSupportsAtomic16() noexcept;
AtomicCompareResult atomicCompareExchange8(void *address, const std::array<std::uint8_t, 16> &expected,
                                           const std::array<std::uint8_t, 16> &desired) noexcept;
AtomicCompareResult atomicCompareExchange16(void *address, const std::array<std::uint8_t, 16> &expected,
                                            const std::array<std::uint8_t, 16> &desired) noexcept;

class AtomicEntryHook {
public:
    AtomicEntryHook() = default;
    ~AtomicEntryHook();

    AtomicEntryHook(const AtomicEntryHook &) = delete;
    AtomicEntryHook &operator=(const AtomicEntryHook &) = delete;

    bool prepare(void *entry, void *hook, std::string &error);
    bool install(std::string &error);
    bool restore(std::string &error);
    bool proveQuiescence(const std::atomic<std::uint64_t> &active_hook_calls, std::uint64_t timeout_ms,
                         std::string &error);
    bool destroy(std::string &error);

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }
    [[nodiscard]] bool installed() const noexcept { return installed_; }
    [[nodiscard]] bool restored() const noexcept { return restored_; }
    [[nodiscard]] bool quiesced() const noexcept { return quiesced_; }
    [[nodiscard]] bool unsafe() const noexcept { return unsafe_; }
    [[nodiscard]] void *trampoline() const noexcept { return trampoline_; }
    [[nodiscard]] void *relay() const noexcept { return relay_; }
    [[nodiscard]] const std::array<std::uint8_t, 16> &originalBytes() const noexcept { return original_; }
    [[nodiscard]] const std::array<std::uint8_t, 16> &installedBytes() const noexcept { return installed_bytes_; }
    [[nodiscard]] std::span<const AddressRange> protectedRanges() const noexcept
    {
        return std::span<const AddressRange>(protected_ranges_.data(), protected_range_count_);
    }
    [[nodiscard]] const char *failureReason() const noexcept { return failure_.data(); }

private:
    bool markFailure(const char *message, std::uint32_t code, std::string &error) noexcept;
    bool markFailureText(const char *message, std::string &error) noexcept;
    bool prepareRelay(std::string &error);
    bool prepareRelocation(std::string &error);
    bool prepareProtectedRanges(std::string &error);
    bool changeEntryProtection(std::uint32_t protection, std::uint32_t &old_protection) noexcept;
    bool restoreEntryProtection(std::uint32_t old_protection) noexcept;
    bool transaction(const std::array<std::uint8_t, 16> &expected,
                     const std::array<std::uint8_t, 16> &desired, bool installing, std::string &error);
    void releasePreparedResources() noexcept;

    void *entry_ = nullptr;
    void *hook_ = nullptr;
    void *relay_ = nullptr;
    void *trampoline_ = nullptr;
    void *relocator_ = nullptr;
    std::array<std::uint8_t, 16> original_{};
    std::array<std::uint8_t, 16> installed_bytes_{};
    std::array<AddressRange, 4> protected_ranges_{};
    std::size_t protected_range_count_ = 0;
    std::array<char, 256> failure_{};
    bool prepared_ = false;
    bool installed_ = false;
    bool restored_ = false;
    bool quiesced_ = false;
    bool unsafe_ = false;
};

#endif

}  // namespace spark::stable_entry_experiment
