#include "native/alloc/windows_permanent_gateway_owned_lifecycle.h"

#ifndef _WIN32
#error "windows_permanent_gateway_owned_lifecycle.cpp must only be compiled on Windows"
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

namespace spark::stable_entry_experiment {
namespace {

constexpr std::uint64_t kOwnerMagic = 0x3152574F47504B53ULL;  // "SKPGOWR1".
constexpr std::uint32_t kOwnerAbiVersion = 1;
constexpr DWORD kOwnerMappingBytes = 4096;
constexpr std::uint64_t kOwnerWaitMs = 5000;
constexpr LONG kOwnerEmpty = 0;
constexpr LONG kOwnerInitializing = 1;
constexpr LONG kOwnerPublished = 2;
constexpr LONG kOwnerUnsafe = 3;

struct alignas(64) OwnershipRecord {
    std::uint64_t magic = 0;
    std::uint32_t abi_version = 0;
    std::uint32_t struct_size = 0;
    std::uint32_t process_id = 0;
    std::uint32_t reserved = 0;
    void *entry = nullptr;
    void *gateway = nullptr;
    void *state = nullptr;
    std::uint64_t fingerprint = 0;
    volatile LONG64 transition = 0;
    volatile LONG64 current_owner = 0;
    volatile LONG64 next_owner = 0;
    volatile LONG64 bound_generation = 0;
    volatile LONG status = kOwnerEmpty;
    LONG reserved_status = 0;
};

static_assert(sizeof(OwnershipRecord) < kOwnerMappingBytes);
static_assert(offsetof(OwnershipRecord, transition) % alignof(LONG64) == 0);
static_assert(offsetof(OwnershipRecord, current_owner) % alignof(LONG64) == 0);
static_assert(offsetof(OwnershipRecord, next_owner) % alignof(LONG64) == 0);
static_assert(offsetof(OwnershipRecord, bound_generation) % alignof(LONG64) == 0);

struct OwnershipView {
    HANDLE mapping = nullptr;
    OwnershipRecord *record = nullptr;
    bool keep_mapping = false;
};

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

[[nodiscard]] std::uint64_t ownershipFingerprint(const OwnershipRecord &record) noexcept
{
    std::uint64_t value = 1469598103934665603ULL;
    value = hashAppend(value, &record.magic, sizeof(record.magic));
    value = hashAppend(value, &record.abi_version, sizeof(record.abi_version));
    value = hashAppend(value, &record.struct_size, sizeof(record.struct_size));
    value = hashAppend(value, &record.process_id, sizeof(record.process_id));
    value = hashAppend(value, &record.entry, sizeof(record.entry));
    value = hashAppend(value, &record.gateway, sizeof(record.gateway));
    value = hashAppend(value, &record.state, sizeof(record.state));
    return value;
}

[[nodiscard]] std::wstring ownershipName(const PermanentGatewayHandle &handle)
{
    return L"Local\\EndstoneSparkPermanentGatewayOwnerV1-" +
           std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(reinterpret_cast<std::uintptr_t>(handle.state));
}

[[nodiscard]] LONG loadStatus(OwnershipRecord *record) noexcept
{
    return ::InterlockedCompareExchange(&record->status, kOwnerEmpty, kOwnerEmpty);
}

[[nodiscard]] LONG64 load64(volatile LONG64 *value) noexcept
{
    return ::InterlockedCompareExchange64(value, 0, 0);
}

void closeOwnershipView(OwnershipView &view) noexcept
{
    if (view.record != nullptr) {
        (void)::UnmapViewOfFile(view.record);
        view.record = nullptr;
    }
    if (view.mapping != nullptr && !view.keep_mapping) {
        (void)::CloseHandle(view.mapping);
    }
    view.mapping = nullptr;
    view.keep_mapping = false;
}

[[nodiscard]] bool validateOwnerMemory(OwnershipRecord *record, std::string &error)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (record == nullptr || ::VirtualQuery(record, &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT || memory.RegionSize < sizeof(OwnershipRecord)) {
        error = "permanent gateway ownership record is not committed memory";
        return false;
    }
    const DWORD protection = memory.Protect & 0xFFU;
    if (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY) {
        error = "permanent gateway ownership record must remain non-executable";
        return false;
    }
    return true;
}

[[nodiscard]] bool waitForPublished(OwnershipRecord *record, std::string &error)
{
    const std::uint64_t deadline = ::GetTickCount64() + kOwnerWaitMs;
    for (;;) {
        const LONG status = loadStatus(record);
        if (status == kOwnerPublished) {
            return true;
        }
        if (status == kOwnerUnsafe) {
            error = "permanent gateway ownership record is marked unsafe";
            return false;
        }
        if (status != kOwnerEmpty && status != kOwnerInitializing) {
            error = "permanent gateway ownership record has an unknown state";
            return false;
        }
        if (::GetTickCount64() >= deadline) {
            error = "timed out waiting for permanent gateway ownership record publication";
            return false;
        }
        ::Sleep(0);
    }
}

[[nodiscard]] bool validatePublishedRecord(OwnershipRecord *record,
                                           const PermanentGatewayHandle &handle,
                                           std::string &error)
{
    if (record->magic != kOwnerMagic || record->abi_version != kOwnerAbiVersion ||
        record->struct_size != sizeof(OwnershipRecord) ||
        record->process_id != ::GetCurrentProcessId() || record->entry != handle.entry ||
        record->gateway != handle.gateway || record->state != handle.state ||
        record->fingerprint != ownershipFingerprint(*record)) {
        error = "permanent gateway ownership identity/fingerprint validation failed";
        return false;
    }
    return true;
}

[[nodiscard]] bool openOwnershipRecord(const PermanentGatewayHandle &handle, OwnershipView &view,
                                       std::string &error)
{
    view = {};
    if (handle.entry == nullptr || handle.gateway == nullptr || handle.state == nullptr) {
        error = "permanent gateway ownership requires a complete gateway handle";
        return false;
    }

    const std::wstring name = ownershipName(handle);
    HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          kOwnerMappingBytes, name.c_str());
    if (mapping == nullptr) {
        error = "CreateFileMappingW permanent gateway ownership failed: " +
                std::to_string(::GetLastError());
        return false;
    }
    const bool created = ::GetLastError() != ERROR_ALREADY_EXISTS;

    auto *record = static_cast<OwnershipRecord *>(
        ::MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kOwnerMappingBytes));
    if (record == nullptr) {
        const DWORD failure = ::GetLastError();
        (void)::CloseHandle(mapping);
        error = "MapViewOfFile permanent gateway ownership failed: " + std::to_string(failure);
        return false;
    }

    view.mapping = mapping;
    view.record = record;
    view.keep_mapping = created;
    if (!validateOwnerMemory(record, error)) {
        closeOwnershipView(view);
        return false;
    }

    if (created) {
        if (::InterlockedCompareExchange(&record->status, kOwnerInitializing, kOwnerEmpty) !=
            kOwnerEmpty) {
            error = "new permanent gateway ownership record was not zero-initialized";
            closeOwnershipView(view);
            return false;
        }
        record->magic = kOwnerMagic;
        record->abi_version = kOwnerAbiVersion;
        record->struct_size = sizeof(OwnershipRecord);
        record->process_id = ::GetCurrentProcessId();
        record->entry = handle.entry;
        record->gateway = handle.gateway;
        record->state = handle.state;
        record->fingerprint = ownershipFingerprint(*record);
        ::InterlockedExchange(&record->status, kOwnerPublished);
        // Keep exactly one mapping handle alive for the process lifetime. The
        // view itself is temporary; later Spark images remap the same record.
        return true;
    }

    if (!waitForPublished(record, error) || !validatePublishedRecord(record, handle, error)) {
        closeOwnershipView(view);
        return false;
    }
    return true;
}

[[nodiscard]] bool claimTransition(OwnershipRecord *record, std::string &error) noexcept
{
    if (::InterlockedCompareExchange64(&record->transition, 1, 0) != 0) {
        error = "permanent gateway lifecycle transition is already in progress";
        return false;
    }
    return true;
}

void releaseTransition(OwnershipRecord *record) noexcept
{
    (void)::InterlockedExchange64(&record->transition, 0);
}

void markUnsafe(OwnershipRecord *record) noexcept
{
    (void)::InterlockedExchange(&record->status, kOwnerUnsafe);
}

}  // namespace

bool bindOwnedPermanentGateway(PermanentGatewayHandle &handle, void *handler, std::uint64_t timeout_ms,
                               PermanentGatewayOwnerTicket &ticket, std::string &error)
{
    ticket = {};
    error.clear();

    OwnershipView view;
    if (!openOwnershipRecord(handle, view, error)) {
        return false;
    }
    OwnershipRecord *record = view.record;
    if (!claimTransition(record, error)) {
        closeOwnershipView(view);
        return false;
    }

    if (loadStatus(record) != kOwnerPublished || load64(&record->current_owner) != 0) {
        error = "permanent gateway is already owned by another handler generation";
        releaseTransition(record);
        closeOwnershipView(view);
        return false;
    }
    if (permanentGatewayAdmissionOpen(handle) || permanentGatewayHandler(handle) != nullptr ||
        permanentGatewayActive(handle) != 0) {
        error = "permanent gateway ownership found an unowned but non-detached gateway; refusing bind";
        markUnsafe(record);
        // Keep transition claimed. The record is now a process-lifetime
        // fail-closed tombstone and no later image may mutate this gateway.
        closeOwnershipView(view);
        return false;
    }

    const LONG64 owner = ::InterlockedIncrement64(&record->next_owner);
    if (owner == 0 || ::InterlockedCompareExchange64(&record->current_owner, owner, 0) != 0) {
        error = "permanent gateway owner ticket exhausted or could not be claimed";
        markUnsafe(record);
        closeOwnershipView(view);
        return false;
    }

    if (!bindPermanentGateway(handle, handler, timeout_ms, error)) {
        (void)::InterlockedCompareExchange64(&record->current_owner, 0, owner);
        releaseTransition(record);
        closeOwnershipView(view);
        return false;
    }

    (void)::InterlockedExchange64(&record->bound_generation,
                                  static_cast<LONG64>(handle.generation));
    ticket.value = static_cast<std::uint64_t>(owner);
    ticket.generation = handle.generation;
    releaseTransition(record);
    closeOwnershipView(view);
    return true;
}

bool detachOwnedPermanentGateway(PermanentGatewayHandle &handle, PermanentGatewayOwnerTicket &ticket,
                                 std::uint64_t timeout_ms, std::string &error)
{
    error.clear();
    if (ticket.value == 0 || ticket.generation == 0) {
        error = "permanent gateway detach requires a live owner ticket";
        return false;
    }

    OwnershipView view;
    if (!openOwnershipRecord(handle, view, error)) {
        return false;
    }
    OwnershipRecord *record = view.record;
    if (!claimTransition(record, error)) {
        closeOwnershipView(view);
        return false;
    }

    const LONG64 owner = static_cast<LONG64>(ticket.value);
    const std::uint64_t persistent_generation =
        static_cast<std::uint64_t>(load64(&record->bound_generation));
    const std::uint64_t current_generation = permanentGatewayGeneration(handle);
    if (loadStatus(record) != kOwnerPublished || load64(&record->current_owner) != owner ||
        persistent_generation != ticket.generation || handle.generation != ticket.generation ||
        current_generation != ticket.generation) {
        error = "permanent gateway detach owner/generation mismatch; stale lifecycle authority rejected";
        releaseTransition(record);
        closeOwnershipView(view);
        return false;
    }

    const bool detached = detachPermanentGateway(handle, timeout_ms, error);
    if (handle.generation != ticket.generation) {
        ticket.generation = handle.generation;
        (void)::InterlockedExchange64(&record->bound_generation,
                                      static_cast<LONG64>(handle.generation));
    }
    if (!detached) {
        // Ownership deliberately remains held. A live caller with this exact
        // ticket may retry; a newer Spark image cannot steal the gateway.
        releaseTransition(record);
        closeOwnershipView(view);
        return false;
    }

    if (permanentGatewayAdmissionOpen(handle) || permanentGatewayActive(handle) != 0 ||
        permanentGatewayHandler(handle) != nullptr) {
        error = "permanent gateway detach returned without a closed/drained/cleared state";
        markUnsafe(record);
        closeOwnershipView(view);
        return false;
    }
    if (::InterlockedCompareExchange64(&record->current_owner, 0, owner) != owner) {
        error = "permanent gateway owner changed while finalizing detach";
        markUnsafe(record);
        closeOwnershipView(view);
        return false;
    }

    ticket.value = 0;
    releaseTransition(record);
    closeOwnershipView(view);
    return true;
}

}  // namespace spark::stable_entry_experiment
