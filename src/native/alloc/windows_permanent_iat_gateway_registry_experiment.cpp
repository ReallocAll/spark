#include "native/alloc/windows_permanent_iat_gateway_registry_experiment.h"

#ifndef _WIN32
#error "windows_permanent_iat_gateway_registry_experiment.cpp must only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace spark::permanent_iat_gateway_experiment {
namespace {

constexpr std::uint64_t kRegistryMagic = 0x3152475441495053ULL;  // "SPIATGR1".
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
    void *original = nullptr;
    void *gateway = nullptr;
    void *state = nullptr;
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
    value = hashAppend(value, &record.original, sizeof(record.original));
    value = hashAppend(value, &record.gateway, sizeof(record.gateway));
    value = hashAppend(value, &record.state, sizeof(record.state));
    return value;
}

[[nodiscard]] std::wstring registryName(void *original)
{
    return L"Local\\EndstoneSparkPermanentIatGatewayRegistryV1-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(reinterpret_cast<std::uintptr_t>(original));
}

[[nodiscard]] LONG registryStatus(RegistryRecord *record) noexcept
{
    return ::InterlockedCompareExchange(&record->status, kRegistryEmpty, kRegistryEmpty);
}

[[nodiscard]] bool validateRegistryMemory(RegistryRecord *record, std::string &error)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (record == nullptr || ::VirtualQuery(record, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        memory.RegionSize < sizeof(RegistryRecord)) {
        error = "permanent IAT gateway registry view is not committed memory";
        return false;
    }
    const DWORD protection = memory.Protect & 0xFFU;
    if (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY) {
        error = "permanent IAT gateway registry must remain non-executable";
        return false;
    }
    return true;
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
            error = "permanent IAT gateway registry contains an unknown state";
            return false;
        }
        if (::GetTickCount64() >= deadline) {
            error = "timed out waiting for permanent IAT gateway registry publication";
            return false;
        }
        ::Sleep(0);
    }
}

[[nodiscard]] bool validatePublishedRecord(RegistryRecord *record, void *original,
                                           std::uint32_t stack_argument_count, std::string &error)
{
    if (record->magic != kRegistryMagic || record->abi_version != kRegistryAbiVersion ||
        record->struct_size != sizeof(RegistryRecord) || record->process_id != ::GetCurrentProcessId() ||
        record->original != original || record->stack_argument_count != stack_argument_count || record->gateway == nullptr ||
        record->state == nullptr || record->fingerprint != registryFingerprint(*record)) {
        error = "permanent IAT gateway registry identity/fingerprint validation failed";
        return false;
    }
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

bool acquirePermanentIatGateway(void *original, std::uint32_t stack_argument_count, PermanentIatGatewayHandle &handle,
                                std::string &error)
{
    handle = {};
    error.clear();
    if (original == nullptr) {
        error = "permanent IAT gateway registry original is null";
        return false;
    }

    const std::wstring name = registryName(original);
    HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kRegistryBytes, name.c_str());
    if (mapping == nullptr) {
        error = "CreateFileMappingW permanent IAT gateway registry failed: " + std::to_string(::GetLastError());
        return false;
    }
    const bool created = ::GetLastError() != ERROR_ALREADY_EXISTS;

    auto *record =
        static_cast<RegistryRecord *>(::MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kRegistryBytes));
    if (record == nullptr) {
        const DWORD failure = ::GetLastError();
        (void)::CloseHandle(mapping);
        error = "MapViewOfFile permanent IAT gateway registry failed: " + std::to_string(failure);
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
            error = "new permanent IAT gateway registry was not zero-initialized";
            return false;
        }

        record->magic = kRegistryMagic;
        record->abi_version = kRegistryAbiVersion;
        record->struct_size = sizeof(RegistryRecord);
        record->process_id = ::GetCurrentProcessId();
        record->stack_argument_count = stack_argument_count;
        record->original = original;

        PermanentIatGatewayHandle created_gateway;
        std::string create_error;
        if (!createPermanentIatGateway(original, stack_argument_count, created_gateway, create_error)) {
            ::InterlockedExchange(&record->status, kRegistryUnsafe);
            error = "permanent IAT gateway first construction failed; registry marked unsafe: " + create_error;
            // Keep the creator mapping/view alive until process exit so a later
            // Spark reload sees the permanent unsafe marker instead of retrying.
            return false;
        }

        record->gateway = created_gateway.gateway;
        record->state = created_gateway.state;
        record->fingerprint = registryFingerprint(*record);
        ::InterlockedExchange(&record->status, kRegistryPublished);

        // The first mapping handle/view intentionally remain alive until process
        // exit. Subsequent acquisitions use temporary handles and never allocate
        // another gateway for this allocator original.
        handle = created_gateway;
        return true;
    }

    LONG status = kRegistryEmpty;
    if (!waitForRegistry(record, status, error)) {
        closeTemporaryRegistry(mapping, record);
        return false;
    }
    if (status == kRegistryUnsafe) {
        closeTemporaryRegistry(mapping, record);
        error = "existing permanent IAT gateway registry is marked unsafe";
        return false;
    }
    if (!validatePublishedRecord(record, original, stack_argument_count, error)) {
        closeTemporaryRegistry(mapping, record);
        return false;
    }

    PermanentIatGatewayHandle discovered;
    if (!discoverPermanentIatGateway(record->gateway, discovered, error)) {
        closeTemporaryRegistry(mapping, record);
        error = "registered permanent IAT gateway failed rediscovery validation: " + error;
        return false;
    }
    if (discovered.gateway != record->gateway || discovered.state != record->state || discovered.original != original ||
        discovered.stack_argument_count != stack_argument_count) {
        closeTemporaryRegistry(mapping, record);
        error = "registered permanent IAT gateway pointers do not match rediscovered gateway";
        return false;
    }

    handle = discovered;
    closeTemporaryRegistry(mapping, record);
    return true;
}

}  // namespace spark::permanent_iat_gateway_experiment
