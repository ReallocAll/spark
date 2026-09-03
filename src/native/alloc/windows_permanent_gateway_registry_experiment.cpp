#include "native/alloc/windows_permanent_gateway_registry_experiment.h"

#ifndef _WIN32
#error "windows_permanent_gateway_registry_experiment.cpp must only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace spark::stable_entry_experiment {
namespace {

constexpr std::uint64_t kRegistryMagic = 0x31474552474B5053ULL;  // "SPKGREG1".
constexpr std::uint32_t kRegistryAbiVersion = 1;
constexpr DWORD kRegistryBytes = 4096;
constexpr std::uint64_t kRegistryWaitMs = 5000;
constexpr LONG kRegistryEmpty = 0;
constexpr LONG kRegistryInitializing = 1;
constexpr LONG kRegistryPublished = 2;
constexpr LONG kRegistryUnsafe = 3;

struct RegistryRecord {
    std::uint64_t magic = 0;
    std::uint32_t abi_version = 0;
    std::uint32_t struct_size = 0;
    std::uint32_t process_id = 0;
    std::uint32_t stack_argument_count = 0;
    void *entry = nullptr;
    void *gateway = nullptr;
    void *original = nullptr;
    void *state = nullptr;
    std::array<std::uint8_t, 8> installed_bytes{};
    std::uint64_t fingerprint = 0;
    volatile LONG status = kRegistryEmpty;
    LONG reserved = 0;
};

static_assert(sizeof(RegistryRecord) < kRegistryBytes);

[[nodiscard]] std::uint64_t hashAppend(std::uint64_t value, const void *data, std::size_t size) noexcept
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (std::size_t index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= prime;
    }
    return value;
}

[[nodiscard]] std::uint64_t registryFingerprint(const RegistryRecord &record) noexcept
{
    std::uint64_t value = 1469598103934665603ULL;
    value = hashAppend(value, &record.magic, sizeof(record.magic));
    value = hashAppend(value, &record.abi_version, sizeof(record.abi_version));
    value = hashAppend(value, &record.struct_size, sizeof(record.struct_size));
    value = hashAppend(value, &record.process_id, sizeof(record.process_id));
    value = hashAppend(value, &record.stack_argument_count, sizeof(record.stack_argument_count));
    value = hashAppend(value, &record.entry, sizeof(record.entry));
    value = hashAppend(value, &record.gateway, sizeof(record.gateway));
    value = hashAppend(value, &record.original, sizeof(record.original));
    value = hashAppend(value, &record.state, sizeof(record.state));
    value = hashAppend(value, record.installed_bytes.data(), record.installed_bytes.size());
    return value;
}

[[nodiscard]] std::wstring registryName(void *entry)
{
    return L"Local\\EndstoneSparkPermanentGatewayRegistryV1-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(reinterpret_cast<std::uintptr_t>(entry));
}

[[nodiscard]] LONG registryStatus(RegistryRecord *record) noexcept
{
    return ::InterlockedCompareExchange(&record->status, kRegistryEmpty, kRegistryEmpty);
}

[[nodiscard]] bool waitForRegistry(RegistryRecord *record, LONG &status, std::string &error)
{
    const std::uint64_t deadline = ::GetTickCount64() + kRegistryWaitMs;
    for (;;) {
        status = registryStatus(record);
        if (status == kRegistryPublished || status == kRegistryUnsafe) {
            return true;
        }
        if (status != kRegistryEmpty && status != kRegistryInitializing) {
            error = "permanent gateway registry contains an unknown state";
            return false;
        }
        if (::GetTickCount64() >= deadline) {
            error = "timed out waiting for permanent gateway registry publication";
            return false;
        }
        ::Sleep(0);
    }
}

[[nodiscard]] bool validateRegistryMemory(RegistryRecord *record, std::string &error)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (record == nullptr || ::VirtualQuery(record, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        memory.RegionSize < sizeof(RegistryRecord)) {
        error = "permanent gateway registry view is not committed memory";
        return false;
    }
    const DWORD protection = memory.Protect & 0xFFU;
    if (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY) {
        error = "permanent gateway registry must remain non-executable";
        return false;
    }
    return true;
}

[[nodiscard]] bool validatePublishedRecord(RegistryRecord *record, void *entry, std::uint32_t stack_argument_count,
                                           std::string &error)
{
    if (record->magic != kRegistryMagic || record->abi_version != kRegistryAbiVersion ||
        record->struct_size != sizeof(RegistryRecord) || record->process_id != ::GetCurrentProcessId() ||
        record->entry != entry || record->stack_argument_count != stack_argument_count || record->gateway == nullptr ||
        record->original == nullptr || record->state == nullptr ||
        record->fingerprint != registryFingerprint(*record)) {
        error = "permanent gateway registry identity/fingerprint validation failed";
        return false;
    }
    return true;
}

[[nodiscard]] bool currentEntryBytes(void *entry, std::array<std::uint8_t, 8> &bytes, std::string &error)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (entry == nullptr || ::VirtualQuery(entry, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT) {
        error = "allocator entry is not committed while validating permanent gateway ownership";
        return false;
    }
    const std::uintptr_t entry_value = reinterpret_cast<std::uintptr_t>(entry);
    const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    if (entry_value > region_end || region_end - entry_value < bytes.size()) {
        error = "allocator entry does not expose the registered 8-byte ownership window";
        return false;
    }
    std::memcpy(bytes.data(), entry, bytes.size());
    return true;
}

void closeTemporaryRegistry(HANDLE mapping, RegistryRecord *record) noexcept
{
    if (record != nullptr) {
        (void)::UnmapViewOfFile(record);
    }
    if (mapping != nullptr) {
        (void)::CloseHandle(mapping);
    }
}

}  // namespace

bool acquirePermanentGateway(void *entry, std::uint32_t stack_argument_count, PermanentGatewayHandle &handle,
                             std::string &error)
{
    handle = {};
    error.clear();
    if (entry == nullptr) {
        error = "permanent gateway registry entry is null";
        return false;
    }

    const std::wstring name = registryName(entry);
    HANDLE mapping =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kRegistryBytes, name.c_str());
    if (mapping == nullptr) {
        error = "CreateFileMappingW permanent gateway registry failed: " + std::to_string(::GetLastError());
        return false;
    }
    const DWORD mapping_status = ::GetLastError();
    const bool created = mapping_status != ERROR_ALREADY_EXISTS;

    auto *record =
        static_cast<RegistryRecord *>(::MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kRegistryBytes));
    if (record == nullptr) {
        const DWORD failure = ::GetLastError();
        (void)::CloseHandle(mapping);
        error = "MapViewOfFile permanent gateway registry failed: " + std::to_string(failure);
        return false;
    }
    if (!validateRegistryMemory(record, error)) {
        closeTemporaryRegistry(mapping, record);
        return false;
    }

    if (created) {
        const LONG previous = ::InterlockedCompareExchange(&record->status, kRegistryInitializing, kRegistryEmpty);
        if (previous != kRegistryEmpty) {
            closeTemporaryRegistry(mapping, record);
            error = "new permanent gateway registry was not zero-initialized";
            return false;
        }

        record->magic = kRegistryMagic;
        record->abi_version = kRegistryAbiVersion;
        record->struct_size = sizeof(RegistryRecord);
        record->process_id = ::GetCurrentProcessId();
        record->stack_argument_count = stack_argument_count;
        record->entry = entry;

        PermanentGatewayHandle installed;
        std::string install_error;
        if (!installPermanentGateway(entry, stack_argument_count, installed, install_error)) {
            // The low-level installer deliberately pins resources after its entry
            // publication boundary even if a later diagnostic fails. The registry
            // therefore stays process-lifetime and permanently refuses a second
            // install whenever first acquisition cannot prove a safe result.
            ::InterlockedExchange(&record->status, kRegistryUnsafe);
            error = "permanent gateway first installation failed; registry marked unsafe: " + install_error;
            // Intentionally retain this first mapping handle and view until process
            // exit. That single bounded leak is the fail-closed lifetime record.
            return false;
        }

        record->gateway = installed.gateway;
        record->original = installed.original;
        record->state = installed.state;
        if (!currentEntryBytes(entry, record->installed_bytes, error)) {
            ::InterlockedExchange(&record->status, kRegistryUnsafe);
            return false;  // retain creator mapping/view process-lifetime
        }
        record->fingerprint = registryFingerprint(*record);
        ::InterlockedExchange(&record->status, kRegistryPublished);

        // The creator handle/view are intentionally not closed or unmapped. They
        // outlive this Spark DLL image and keep the named registry available to a
        // future hot-reloaded image. Reload acquisitions below are temporary and
        // are always closed, so this is one bounded process-lifetime resource per
        // allocator entry rather than one leak per reload.
        handle = installed;
        return true;
    }

    LONG status = kRegistryEmpty;
    if (!waitForRegistry(record, status, error)) {
        closeTemporaryRegistry(mapping, record);
        return false;
    }
    if (status == kRegistryUnsafe) {
        closeTemporaryRegistry(mapping, record);
        error = "existing permanent gateway registry is marked unsafe; refusing second installation";
        return false;
    }
    if (!validatePublishedRecord(record, entry, stack_argument_count, error)) {
        closeTemporaryRegistry(mapping, record);
        return false;
    }

    std::array<std::uint8_t, 8> current{};
    if (!currentEntryBytes(entry, current, error)) {
        closeTemporaryRegistry(mapping, record);
        return false;
    }
    if (current != record->installed_bytes) {
        closeTemporaryRegistry(mapping, record);
        error = "existing process-lifetime gateway registry found but allocator entry ownership is lost; refusing "
                "second island";
        return false;
    }

    PermanentGatewayHandle discovered;
    if (!discoverPermanentGateway(entry, discovered, error)) {
        closeTemporaryRegistry(mapping, record);
        error = "registered permanent gateway failed full rediscovery validation: " + error;
        return false;
    }
    if (discovered.gateway != record->gateway || discovered.original != record->original ||
        discovered.state != record->state || discovered.stack_argument_count != record->stack_argument_count) {
        closeTemporaryRegistry(mapping, record);
        error = "registered permanent gateway pointers do not match rediscovered gateway";
        return false;
    }

    handle = discovered;
    closeTemporaryRegistry(mapping, record);
    return true;
}

}  // namespace spark::stable_entry_experiment
