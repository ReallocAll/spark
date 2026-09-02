#include "native/alloc/windows_stable_entry_atomic.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>

namespace spark::stable_entry_experiment {

bool isAlignedForAtomic8(std::uintptr_t address) noexcept
{
    return (address & (kAtomicEntryWidth8 - 1)) == 0;
}

bool isAlignedForAtomic16(std::uintptr_t address) noexcept
{
    return (address & (kAtomicEntryWidth16 - 1)) == 0;
}

bool rel32Reachable(std::uintptr_t instruction_end, std::uintptr_t destination) noexcept
{
    if (destination >= instruction_end) {
        return destination - instruction_end <= static_cast<std::uintptr_t>(INT32_MAX);
    }
    return instruction_end - destination <= static_cast<std::uintptr_t>(INT32_MAX) + 1ULL;
}

bool encodeAtomic8RelayEntry(std::uintptr_t entry, std::uintptr_t relay,
                             const std::array<std::uint8_t, 16> &original,
                             std::array<std::uint8_t, 16> &installed, std::string &error)
{
    error.clear();
    if (!isAlignedForAtomic8(entry)) {
        error = "stable entry is not 8-byte aligned";
        return false;
    }
    if (!rel32Reachable(entry + 5, relay)) {
        error = "stable entry relay is outside rel32 range";
        return false;
    }

    installed = original;
    const std::int64_t displacement = static_cast<std::int64_t>(relay) - static_cast<std::int64_t>(entry + 5);
    const std::int32_t rel32 = static_cast<std::int32_t>(displacement);
    installed[0] = 0xE9;
    std::memcpy(installed.data() + 1, &rel32, sizeof(rel32));
    // Bytes 5..7 deliberately remain exactly original. The published 8-byte
    // transaction therefore changes only the rel32 jump while still making the
    // entire observer-visible write one naturally aligned atomic operation.
    return true;
}

bool encodeAtomic16AbsoluteEntry(std::uintptr_t entry, std::uintptr_t hook,
                                 const std::array<std::uint8_t, 16> &original,
                                 std::array<std::uint8_t, 16> &installed, std::string &error)
{
    error.clear();
    if (!isAlignedForAtomic16(entry)) {
        error = "stable entry is not 16-byte aligned";
        return false;
    }
    installed = original;
    installed[0] = 0xFF;
    installed[1] = 0x25;
    installed[2] = 0x00;
    installed[3] = 0x00;
    installed[4] = 0x00;
    installed[5] = 0x00;
    const std::uint64_t target = static_cast<std::uint64_t>(hook);
    std::memcpy(installed.data() + 6, &target, sizeof(target));
    installed[14] = 0x90;
    installed[15] = 0x90;
    return true;
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows SDK headers require windows.h first.
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
// clang-format on

#include <array>

#include <funchook.h>

namespace {

constexpr std::size_t kMaxQuiescenceThreads = 4096;
constexpr std::size_t kRelayReservationSize = 64 * 1024;

struct SuspendedThread {
    HANDLE handle = nullptr;
    DWORD id = 0;
    DWORD previous_suspend_count = 0;
    bool suspended = false;
};

bool addressInRanges(std::uintptr_t address, std::span<const AddressRange> ranges) noexcept
{
    return std::ranges::any_of(ranges, [address](const AddressRange &range) { return range.contains(address); });
}

void resumeOrTerminate(std::array<SuspendedThread, kMaxQuiescenceThreads> &threads, std::size_t count) noexcept
{
    for (std::size_t index = count; index != 0; --index) {
        SuspendedThread &thread = threads[index - 1];
        if (!thread.suspended || thread.handle == nullptr) {
            continue;
        }
        DWORD failure = ERROR_SUCCESS;
        bool resumed = false;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            if (::ResumeThread(thread.handle) != static_cast<DWORD>(-1)) {
                resumed = true;
                thread.suspended = false;
                break;
            }
            failure = ::GetLastError();
            DWORD exit_code = STILL_ACTIVE;
            if (::GetExitCodeThread(thread.handle, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                resumed = true;
                thread.suspended = false;
                break;
            }
            ::Sleep(1);
        }
        if (!resumed) {
            ::TerminateProcess(::GetCurrentProcess(), failure != ERROR_SUCCESS ? failure : ERROR_GEN_FAILURE);
            std::abort();
        }
    }
}

void closeThreadHandles(std::array<SuspendedThread, kMaxQuiescenceThreads> &threads, std::size_t count) noexcept
{
    for (std::size_t index = 0; index < count; ++index) {
        if (threads[index].handle != nullptr) {
            ::CloseHandle(threads[index].handle);
            threads[index].handle = nullptr;
        }
    }
}

bool dynamicCodeAllowed() noexcept
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy{};
    return ::GetProcessMitigationPolicy(
               ::GetCurrentProcess(), ProcessDynamicCodePolicy, &policy, static_cast<SIZE_T>(sizeof(policy))) != FALSE &&
           policy.ProhibitDynamicCode == 0;
}

}  // namespace

bool cpuSupportsAtomic16() noexcept
{
#ifdef PF_COMPARE_EXCHANGE128
    return ::IsProcessorFeaturePresent(PF_COMPARE_EXCHANGE128) != FALSE;
#else
    return false;
#endif
}

AtomicCompareResult atomicCompareExchange8(void *address, const std::array<std::uint8_t, 16> &expected,
                                           const std::array<std::uint8_t, 16> &desired) noexcept
{
    AtomicCompareResult result;
    if (address == nullptr || !isAlignedForAtomic8(reinterpret_cast<std::uintptr_t>(address))) {
        return result;
    }
    long long expected_word = 0;
    long long desired_word = 0;
    std::memcpy(&expected_word, expected.data(), sizeof(expected_word));
    std::memcpy(&desired_word, desired.data(), sizeof(desired_word));
    const long long observed = _InterlockedCompareExchange64(reinterpret_cast<volatile long long *>(address),
                                                              desired_word, expected_word);
    std::memcpy(result.observed.data(), &observed, sizeof(observed));
    result.exchanged = observed == expected_word;
    return result;
}

AtomicCompareResult atomicCompareExchange16(void *address, const std::array<std::uint8_t, 16> &expected,
                                            const std::array<std::uint8_t, 16> &desired) noexcept
{
    AtomicCompareResult result;
    if (address == nullptr || !cpuSupportsAtomic16() ||
        !isAlignedForAtomic16(reinterpret_cast<std::uintptr_t>(address))) {
        return result;
    }
    long long expected_words[2]{};
    long long desired_words[2]{};
    std::memcpy(expected_words, expected.data(), sizeof(expected_words));
    std::memcpy(desired_words, desired.data(), sizeof(desired_words));
    long long comparand[2]{expected_words[0], expected_words[1]};
    result.exchanged = _InterlockedCompareExchange128(reinterpret_cast<volatile long long *>(address), desired_words[1],
                                                       desired_words[0], comparand) != 0;
    std::memcpy(result.observed.data(), comparand, sizeof(comparand));
    return result;
}

AtomicEntryHook::~AtomicEntryHook()
{
    // Never reclaim executable memory when ownership/quiescence is uncertain.
    // Leaking an experimental trampoline is fail-closed; freeing code that a
    // stale instruction pointer may execute is not.
    if (!unsafe_ && !installed_ && (!prepared_ || (restored_ && quiesced_))) {
        releasePreparedResources();
    }
}

bool AtomicEntryHook::markFailure(const char *message, std::uint32_t code, std::string &error) noexcept
{
    unsafe_ = true;
    std::snprintf(failure_.data(), failure_.size(), "%s (error=%lu)", message,
                  static_cast<unsigned long>(code));
    try {
        error.assign(failure_.data());
    }
    catch (...) {
        error.clear();
    }
    return false;
}

bool AtomicEntryHook::markFailureText(const char *message, std::string &error) noexcept
{
    unsafe_ = true;
    std::snprintf(failure_.data(), failure_.size(), "%s", message);
    try {
        error.assign(failure_.data());
    }
    catch (...) {
        error.clear();
    }
    return false;
}

bool AtomicEntryHook::prepareRelay(std::string &error)
{
    SYSTEM_INFO system{};
    ::GetSystemInfo(&system);
    const std::uintptr_t granularity = system.dwAllocationGranularity != 0 ? system.dwAllocationGranularity
                                                                           : kRelayReservationSize;
    const std::uintptr_t entry = reinterpret_cast<std::uintptr_t>(entry_);
    const std::uintptr_t base = entry & ~(granularity - 1);
    const std::uintptr_t max_distance = static_cast<std::uintptr_t>(INT32_MAX) - granularity;

    for (std::uintptr_t distance = 0; distance <= max_distance; distance += granularity) {
        const std::uintptr_t candidates[2] = {
            base >= distance ? base - distance : 0,
            base <= (std::numeric_limits<std::uintptr_t>::max)() - distance ? base + distance : 0,
        };
        for (std::uintptr_t candidate : candidates) {
            if (candidate == 0 || !rel32Reachable(entry + 5, candidate)) {
                continue;
            }
            void *memory = ::VirtualAlloc(reinterpret_cast<void *>(candidate), kRelayReservationSize,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (memory == nullptr || reinterpret_cast<std::uintptr_t>(memory) != candidate) {
                if (memory != nullptr) {
                    ::VirtualFree(memory, 0, MEM_RELEASE);
                }
                continue;
            }
            relay_ = memory;
            std::array<std::uint8_t, kAbsoluteIndirectJumpSize> code{};
            code[0] = 0xFF;
            code[1] = 0x25;
            const std::uint64_t target = reinterpret_cast<std::uint64_t>(hook_);
            std::memcpy(code.data() + 6, &target, sizeof(target));
            std::memcpy(relay_, code.data(), code.size());
            DWORD old_protection = 0;
            if (::VirtualProtect(relay_, kRelayReservationSize, PAGE_EXECUTE_READ, &old_protection) == FALSE) {
                const DWORD failure = ::GetLastError();
                ::VirtualFree(relay_, 0, MEM_RELEASE);
                relay_ = nullptr;
                return markFailure("VirtualProtect relay executable failed", failure, error);
            }
            if (::FlushInstructionCache(::GetCurrentProcess(), relay_, code.size()) == FALSE) {
                const DWORD failure = ::GetLastError();
                ::VirtualFree(relay_, 0, MEM_RELEASE);
                relay_ = nullptr;
                return markFailure("FlushInstructionCache relay failed", failure, error);
            }
            return true;
        }
        if (max_distance - distance < granularity) {
            break;
        }
    }
    return markFailureText("could not reserve a rel32-reachable stable entry relay", error);
}

bool AtomicEntryHook::prepareRelocation(std::string &error)
{
    funchook_t *relocator = funchook_create();
    if (relocator == nullptr) {
        return markFailureText("funchook_create failed while preparing stable-entry relocation", error);
    }

    void *callable = entry_;
    // We deliberately do NOT give funchook a Spark hook pointer. Its prepare-only
    // role is bounded instruction relocation and trampoline allocation; the
    // actual public Spark pointer exists solely in our relay. This prevents an
    // unused funchook transit stub from becoming an external stale Spark pointer.
    const int code = funchook_prepare(relocator, &callable, entry_);
    if (code != FUNCHOOK_ERROR_SUCCESS || callable == nullptr || callable == entry_) {
        const char *detail = funchook_error_message(relocator);
        std::snprintf(failure_.data(), failure_.size(), "stable-entry relocation failed (code=%d): %s", code,
                      detail != nullptr ? detail : "no detail");
        funchook_destroy(relocator);
        unsafe_ = true;
        error = failure_.data();
        return false;
    }

    // Upstream funchook makes prepared trampoline pages executable only from
    // funchook_install(). Stable Entry must never call that installer because it
    // publishes the target JMP with a normal memcpy. Convert just the prepared
    // trampoline allocation to RX here, before the pointer is published, while
    // Spark still owns the executable transaction and lifecycle boundary.
    MEMORY_BASIC_INFORMATION trampoline_memory{};
    if (::VirtualQuery(callable, &trampoline_memory, sizeof(trampoline_memory)) == 0 ||
        trampoline_memory.BaseAddress == nullptr || trampoline_memory.RegionSize == 0 ||
        trampoline_memory.State != MEM_COMMIT) {
        const DWORD failure = ::GetLastError();
        (void)funchook_destroy(relocator);
        return markFailure("VirtualQuery prepared stable-entry trampoline failed", failure, error);
    }
    DWORD old_protection = 0;
    if (::VirtualProtect(trampoline_memory.BaseAddress, trampoline_memory.RegionSize, PAGE_EXECUTE_READ,
                         &old_protection) == FALSE) {
        const DWORD failure = ::GetLastError();
        (void)funchook_destroy(relocator);
        return markFailure("VirtualProtect prepared stable-entry trampoline executable failed", failure, error);
    }
    if (::FlushInstructionCache(::GetCurrentProcess(), trampoline_memory.BaseAddress, trampoline_memory.RegionSize) ==
        FALSE) {
        const DWORD failure = ::GetLastError();
        (void)funchook_destroy(relocator);
        return markFailure("FlushInstructionCache prepared stable-entry trampoline failed", failure, error);
    }

    relocator_ = relocator;
    trampoline_ = callable;
    return true;
}

bool AtomicEntryHook::prepareProtectedRanges(std::string &error)
{
    protected_range_count_ = 0;
    protected_ranges_[protected_range_count_++] = {
        reinterpret_cast<std::uintptr_t>(entry_), reinterpret_cast<std::uintptr_t>(entry_) + kAtomicEntryWidth8};
    protected_ranges_[protected_range_count_++] = {
        reinterpret_cast<std::uintptr_t>(relay_), reinterpret_cast<std::uintptr_t>(relay_) + kAbsoluteIndirectJumpSize};

    MEMORY_BASIC_INFORMATION trampoline_memory{};
    if (::VirtualQuery(trampoline_, &trampoline_memory, sizeof(trampoline_memory)) == 0 ||
        trampoline_memory.BaseAddress == nullptr || trampoline_memory.RegionSize == 0) {
        return markFailure("VirtualQuery trampoline failed", ::GetLastError(), error);
    }
    protected_ranges_[protected_range_count_++] = {
        reinterpret_cast<std::uintptr_t>(trampoline_memory.BaseAddress),
        reinterpret_cast<std::uintptr_t>(trampoline_memory.BaseAddress) + trampoline_memory.RegionSize};

#if defined(_M_X64) || defined(__x86_64__)
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function =
        ::RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(hook_), &image_base, nullptr);
    if (function == nullptr || function->BeginAddress >= function->EndAddress) {
        return markFailureText("RtlLookupFunctionEntry could not bound the Spark hook pre-guard corridor", error);
    }
    protected_ranges_[protected_range_count_++] = {
        static_cast<std::uintptr_t>(image_base + function->BeginAddress),
        static_cast<std::uintptr_t>(image_base + function->EndAddress)};
#else
    return markFailureText("stable-entry experiment currently requires Windows x64 unwind metadata", error);
#endif
    return true;
}

bool AtomicEntryHook::prepare(void *entry, void *hook, std::string &error)
{
    error.clear();
    if (prepared_ || entry == nullptr || hook == nullptr) {
        return markFailureText("stable-entry prepare received invalid lifecycle state or address", error);
    }
    if (!dynamicCodeAllowed()) {
        return markFailureText("ProcessDynamicCodePolicy prohibits stable-entry executable modification", error);
    }
    entry_ = entry;
    hook_ = hook;
    const std::uintptr_t entry_address = reinterpret_cast<std::uintptr_t>(entry_);
    if (!isAlignedForAtomic8(entry_address)) {
        return markFailureText("stable entry is not 8-byte aligned; atomic8 strategy unavailable", error);
    }

    MEMORY_BASIC_INFORMATION memory{};
    if (::VirtualQuery(entry_, &memory, sizeof(memory)) == 0 || memory.BaseAddress == nullptr) {
        return markFailure("VirtualQuery stable entry failed", ::GetLastError(), error);
    }
    const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    if (entry_address > region_end || region_end - entry_address < original_.size()) {
        return markFailureText("stable entry does not expose a bounded 16-byte readable window", error);
    }
    std::memcpy(original_.data(), entry_, original_.size());

    if (!prepareRelocation(error)) {
        return false;
    }
    if (!prepareRelay(error)) {
        return false;
    }
    if (!encodeAtomic8RelayEntry(entry_address, reinterpret_cast<std::uintptr_t>(relay_), original_, installed_bytes_,
                                 error)) {
        unsafe_ = true;
        std::snprintf(failure_.data(), failure_.size(), "%s", error.c_str());
        return false;
    }
    if (!prepareProtectedRanges(error)) {
        return false;
    }
    prepared_ = true;
    restored_ = false;
    quiesced_ = false;
    unsafe_ = false;
    failure_.fill('\0');
    return true;
}

bool AtomicEntryHook::changeEntryProtection(std::uint32_t protection, std::uint32_t &old_protection) noexcept
{
    DWORD old_value = 0;
    const BOOL ok = ::VirtualProtect(entry_, kAtomicEntryWidth8, static_cast<DWORD>(protection), &old_value);
    old_protection = static_cast<std::uint32_t>(old_value);
    return ok != FALSE;
}

bool AtomicEntryHook::restoreEntryProtection(std::uint32_t old_protection) noexcept
{
    DWORD ignored = 0;
    return ::VirtualProtect(entry_, kAtomicEntryWidth8, static_cast<DWORD>(old_protection), &ignored) != FALSE;
}

bool AtomicEntryHook::transaction(const std::array<std::uint8_t, 16> &expected,
                                  const std::array<std::uint8_t, 16> &desired, bool installing,
                                  std::string &error)
{
    std::uint32_t old_protection = 0;
    if (!changeEntryProtection(PAGE_EXECUTE_READWRITE, old_protection)) {
        return markFailure(installing ? "VirtualProtect before atomic install failed"
                                     : "VirtualProtect before atomic restore failed",
                           ::GetLastError(), error);
    }

    const AtomicCompareResult result = atomicCompareExchange8(entry_, expected, desired);
    if (!result.exchanged) {
        const bool protection_restored = restoreEntryProtection(old_protection);
        if (!protection_restored) {
            return markFailure("stable-entry ownership mismatch and protection restore failed", ::GetLastError(), error);
        }
        return markFailureText(installing ? "stable-entry original ownership lost before atomic install"
                                          : "stable-entry installed ownership lost before atomic restore",
                               error);
    }

    // The target bytes are now fully old or fully new; no observer can see a
    // partially published JMP. FlushInstructionCache establishes Windows'
    // documented code-modification visibility boundary before teardown proceeds.
    if (::FlushInstructionCache(::GetCurrentProcess(), entry_, kAtomicEntryWidth8) == FALSE) {
        const DWORD failure = ::GetLastError();
        (void)restoreEntryProtection(old_protection);
        return markFailure(installing ? "FlushInstructionCache after atomic install failed"
                                     : "FlushInstructionCache after atomic restore failed",
                           failure, error);
    }
    if (!restoreEntryProtection(old_protection)) {
        return markFailure(installing ? "VirtualProtect restore after atomic install failed"
                                     : "VirtualProtect restore after atomic entry restore failed",
                           ::GetLastError(), error);
    }
    return true;
}

bool AtomicEntryHook::install(std::string &error)
{
    error.clear();
    if (!prepared_ || installed_ || unsafe_) {
        return markFailureText("atomic stable-entry install is invalid in current lifecycle state", error);
    }
    if (!transaction(original_, installed_bytes_, true, error)) {
        return false;
    }
    installed_ = true;
    restored_ = false;
    quiesced_ = false;
    return true;
}

bool AtomicEntryHook::restore(std::string &error)
{
    error.clear();
    if (!prepared_ || !installed_ || unsafe_) {
        return markFailureText("atomic stable-entry restore is invalid in current lifecycle state", error);
    }
    if (!transaction(installed_bytes_, original_, false, error)) {
        return false;
    }
    installed_ = false;
    restored_ = true;
    quiesced_ = false;
    return true;
}

bool AtomicEntryHook::proveQuiescence(const std::atomic<std::uint64_t> &active_hook_calls, std::uint64_t timeout_ms,
                                      std::string &error)
{
    error.clear();
    if (!prepared_ || installed_ || !restored_ || unsafe_) {
        return markFailureText("post-restore quiescence proof is invalid in current lifecycle state", error);
    }

    const std::uint64_t deadline = ::GetTickCount64() + timeout_ms;
    while (true) {
        std::array<SuspendedThread, kMaxQuiescenceThreads> threads{};
        std::size_t count = 0;
        DWORD failure = ERROR_SUCCESS;
        DWORD failed_thread = 0;
        bool protected_rip = false;

        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return markFailure("CreateToolhelp32Snapshot after atomic restore failed", ::GetLastError(), error);
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (::Thread32First(snapshot, &entry) == FALSE) {
            failure = ::GetLastError();
        }
        else {
            const DWORD process_id = ::GetCurrentProcessId();
            const DWORD current_thread = ::GetCurrentThreadId();
            do {
                if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == current_thread) {
                    continue;
                }
                if (count == threads.size()) {
                    failure = ERROR_NOT_ENOUGH_MEMORY;
                    break;
                }
                HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                             FALSE, entry.th32ThreadID);
                if (thread == nullptr) {
                    const DWORD code = ::GetLastError();
                    if (code == ERROR_INVALID_PARAMETER) {
                        continue;
                    }
                    failure = code;
                    failed_thread = entry.th32ThreadID;
                    break;
                }
                const DWORD previous = ::SuspendThread(thread);
                if (previous == static_cast<DWORD>(-1)) {
                    const DWORD code = ::GetLastError();
                    DWORD exit_code = STILL_ACTIVE;
                    if (::GetExitCodeThread(thread, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                        ::CloseHandle(thread);
                        continue;
                    }
                    ::CloseHandle(thread);
                    failure = code;
                    failed_thread = entry.th32ThreadID;
                    break;
                }
                threads[count++] = {.handle = thread,
                                    .id = entry.th32ThreadID,
                                    .previous_suspend_count = previous,
                                    .suspended = true};
            } while (::Thread32Next(snapshot, &entry) != FALSE);
            if (failure == ERROR_SUCCESS) {
                const DWORD iteration_error = ::GetLastError();
                if (iteration_error != ERROR_NO_MORE_FILES) {
                    failure = iteration_error;
                }
            }
        }
        ::CloseHandle(snapshot);

        if (failure == ERROR_SUCCESS) {
            for (std::size_t index = 0; index < count; ++index) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (::GetThreadContext(threads[index].handle, &context) == FALSE) {
                    DWORD exit_code = STILL_ACTIVE;
                    if (::GetExitCodeThread(threads[index].handle, &exit_code) != FALSE && exit_code != STILL_ACTIVE) {
                        continue;
                    }
                    failure = ::GetLastError();
                    failed_thread = threads[index].id;
                    break;
                }
#if defined(_M_X64) || defined(__x86_64__)
                const std::uintptr_t rip = static_cast<std::uintptr_t>(context.Rip);
#else
#error "stable entry experiment currently supports Windows x64 only"
#endif
                if (addressInRanges(rip, protectedRanges())) {
                    protected_rip = true;
                    break;
                }
            }
        }

        const bool active = active_hook_calls.load(std::memory_order_acquire) != 0;
        resumeOrTerminate(threads, count);
        closeThreadHandles(threads, count);

        if (failure != ERROR_SUCCESS) {
            if (::GetTickCount64() >= deadline) {
                std::snprintf(failure_.data(), failure_.size(),
                              "post-restore quiescence could not inspect thread %lu (error=%lu)",
                              static_cast<unsigned long>(failed_thread), static_cast<unsigned long>(failure));
                unsafe_ = true;
                error = failure_.data();
                return false;
            }
            ::Sleep(1);
            continue;
        }
        if (!protected_rip && !active) {
            // Unlike pre-restore patching, no second thread enumeration is a
            // safety invariant here. The atomic restore has already cut the only
            // public path into Spark, so a newly created thread can only execute
            // the restored allocator entry and cannot create a new callback.
            quiesced_ = true;
            return true;
        }
        if (::GetTickCount64() >= deadline) {
            return markFailureText("timed out draining post-restore stable-entry executable ranges", error);
        }
        ::Sleep(1);
    }
}

bool AtomicEntryHook::destroy(std::string &error)
{
    error.clear();
    if (!prepared_ || installed_ || !restored_ || !quiesced_ || unsafe_) {
        return markFailureText("cannot reclaim stable-entry executable resources before restore and quiescence", error);
    }
    if (relocator_ != nullptr) {
        const int code = funchook_destroy(static_cast<funchook_t *>(relocator_));
        if (code != FUNCHOOK_ERROR_SUCCESS) {
            return markFailureText("funchook_destroy failed after stable-entry quiescence", error);
        }
        relocator_ = nullptr;
        trampoline_ = nullptr;
    }
    if (relay_ != nullptr && ::VirtualFree(relay_, 0, MEM_RELEASE) == FALSE) {
        return markFailure("VirtualFree stable-entry relay failed", ::GetLastError(), error);
    }
    relay_ = nullptr;
    entry_ = nullptr;
    hook_ = nullptr;
    protected_range_count_ = 0;
    prepared_ = false;
    restored_ = false;
    quiesced_ = false;
    return true;
}

void AtomicEntryHook::releasePreparedResources() noexcept
{
    if (relocator_ != nullptr) {
        (void)funchook_destroy(static_cast<funchook_t *>(relocator_));
        relocator_ = nullptr;
        trampoline_ = nullptr;
    }
    if (relay_ != nullptr) {
        (void)::VirtualFree(relay_, 0, MEM_RELEASE);
        relay_ = nullptr;
    }
}

#endif

}  // namespace spark::stable_entry_experiment
