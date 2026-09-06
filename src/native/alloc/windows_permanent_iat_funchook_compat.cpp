#include <funchook.h>

#ifndef _WIN32
#error "windows_permanent_iat_funchook_compat.cpp must only be compiled on Windows"
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native/alloc/windows_iat_hooks.h"
#include "native/alloc/windows_permanent_iat_gateway_registry_experiment.h"

namespace {

constexpr std::uint64_t kGatewayDrainTimeoutMs = 5000;

struct HookRecord {
    const char *name = nullptr;
    void *original = nullptr;
    void *handler = nullptr;
    bool required_coverage = false;
    std::uint32_t stack_argument_count = 0;
    spark::permanent_iat_gateway_experiment::PermanentIatGatewayHandle gateway;
};

struct TargetDescription {
    const char *name = nullptr;
    bool required_coverage = false;
    std::uint32_t stack_argument_count = 0;
};

bool sameExport(HMODULE module, const char *name, void *address) noexcept
{
    return module != nullptr && ::GetProcAddress(module, name) == address;
}

bool describeTarget(void *address, TargetDescription &description) noexcept
{
    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const struct Candidate {
        HMODULE module;
        const char *name;
        bool required_coverage;
        std::uint32_t stack_argument_count;
    } candidates[] = {
        {.module = ucrt, .name = "malloc", .required_coverage = true, .stack_argument_count = 0},
        {.module = ucrt, .name = "calloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "realloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_recalloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "free", .required_coverage = true, .stack_argument_count = 0},
        {.module = ucrt, .name = "_aligned_malloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_aligned_realloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_aligned_recalloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_aligned_offset_malloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_aligned_offset_realloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_aligned_offset_recalloc", .required_coverage = false, .stack_argument_count = 1},
        {.module = ucrt, .name = "_aligned_free", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_malloc_base", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_calloc_base", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_realloc_base", .required_coverage = false, .stack_argument_count = 0},
        {.module = ucrt, .name = "_free_base", .required_coverage = false, .stack_argument_count = 0},
        {.module = kernel32, .name = "HeapAlloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = kernel32, .name = "HeapReAlloc", .required_coverage = false, .stack_argument_count = 0},
        {.module = kernel32, .name = "HeapFree", .required_coverage = false, .stack_argument_count = 0},
    };
    for (const Candidate &candidate : candidates) {
        if (sameExport(candidate.module, candidate.name, address)) {
            description = {.name = candidate.name,
                           .required_coverage = candidate.required_coverage,
                           .stack_argument_count = candidate.stack_argument_count};
            return true;
        }
    }
    return false;
}

std::vector<spark::WindowsIatHookTarget> makeTargets(const std::vector<HookRecord> &records)
{
    std::vector<spark::WindowsIatHookTarget> targets;
    targets.reserve(records.size());
    for (const HookRecord &record : records) {
        targets.push_back({.import_name = record.name,
                           .import_modules = {},
                           .original = record.original,
                           .replacement = record.gateway.gateway,
                           .required = record.required_coverage});
    }
    return targets;
}

bool acquireGateways(std::vector<HookRecord> &records, std::string &error)
{
    for (HookRecord &record : records) {
        if (!spark::permanent_iat_gateway_experiment::acquirePermanentIatGateway(
                record.original, record.stack_argument_count, record.gateway, error)) {
            return false;
        }
        if (spark::permanent_iat_gateway_experiment::permanentIatGatewayAdmissionOpen(record.gateway) ||
            spark::permanent_iat_gateway_experiment::permanentIatGatewayHandler(record.gateway) != nullptr) {
            error = std::string("permanent IAT gateway is still bound from an earlier Spark image: ") + record.name;
            return false;
        }
    }
    return true;
}

bool normalizePreviousGatewaySlots(const std::vector<HookRecord> &records, std::string &error)
{
    auto backend = spark::makeNativeWindowsIatHookBackend(reinterpret_cast<void *>(&funchook_install));
    if (backend == nullptr) {
        error = "native Windows IAT hook backend is unavailable during permanent-gateway normalization";
        return false;
    }

    const std::vector<spark::WindowsIatHookTarget> targets = makeTargets(records);
    std::vector<spark::WindowsIatSlot> slots;
    if (!backend->enumerate(targets, slots, error)) {
        return false;
    }

    for (const spark::WindowsIatSlot &slot : slots) {
        if (slot.target_index >= targets.size()) {
            error = "native Windows IAT backend returned an invalid target index during gateway normalization";
            return false;
        }
        const spark::WindowsIatHookTarget &target = targets[slot.target_index];
        void *current = nullptr;
        std::string access_error;
        const spark::WindowsIatAccessStatus read_status = backend->read(slot, current, access_error);
        if (read_status == spark::WindowsIatAccessStatus::Stale) {
            continue;
        }
        if (read_status == spark::WindowsIatAccessStatus::Error) {
            error = access_error.empty() ? "failed to inspect a previous permanent IAT gateway slot" : access_error;
            return false;
        }
        if (current != target.replacement) {
            continue;
        }

        std::string exchange_error;
        const spark::WindowsIatExchangeResult exchange =
            backend->compareExchange(slot, target.replacement, target.original, exchange_error);
        if (exchange.status == spark::WindowsIatExchangeStatus::Exchanged ||
            exchange.status == spark::WindowsIatExchangeStatus::Stale ||
            (exchange.status == spark::WindowsIatExchangeStatus::Mismatch && exchange.observed != target.replacement)) {
            continue;
        }

        current = nullptr;
        access_error.clear();
        const spark::WindowsIatAccessStatus after_status = backend->read(slot, current, access_error);
        if (after_status == spark::WindowsIatAccessStatus::Stale ||
            (after_status == spark::WindowsIatAccessStatus::Accessible && current != target.replacement)) {
            continue;
        }
        error = exchange_error.empty() ? "previous permanent IAT gateway slot could not be normalized" : exchange_error;
        return false;
    }
    return true;
}

bool detachGateways(std::vector<HookRecord> &records, std::string &error) noexcept
{
    for (HookRecord &record : records) {
        if (record.gateway.state == nullptr) {
            continue;
        }
        if (!spark::permanent_iat_gateway_experiment::permanentIatGatewayAdmissionOpen(record.gateway) &&
            spark::permanent_iat_gateway_experiment::permanentIatGatewayHandler(record.gateway) == nullptr) {
            continue;
        }
        std::string detach_error;
        if (!spark::permanent_iat_gateway_experiment::detachPermanentIatGateway(record.gateway, kGatewayDrainTimeoutMs,
                                                                                detach_error)) {
            try {
                error = std::string("failed to detach permanent IAT gateway ") + record.name + ": " + detach_error;
            }
            catch (...) {
                error.clear();
            }
            return false;
        }
    }
    return true;
}

}  // namespace

struct funchook {
    std::vector<HookRecord> records;
    std::unique_ptr<spark::WindowsIatHooks> hooks;
    bool installed = false;
    std::string error;
};

extern "C" funchook_t *funchook_create(void)
{
    try {
        return new funchook_t{};
    }
    catch (...) {
        return nullptr;
    }
}

extern "C" int funchook_prepare(funchook_t *funchook, void **target_func, void *hook_func)
{
    if (funchook == nullptr || target_func == nullptr || *target_func == nullptr || hook_func == nullptr) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    try {
        TargetDescription description;
        if (!describeTarget(*target_func, description)) {
            funchook->error = "unsupported Windows allocation hook target";
            return FUNCHOOK_ERROR_INTERNAL;
        }
        funchook->records.push_back({.name = description.name,
                                     .original = *target_func,
                                     .handler = hook_func,
                                     .required_coverage = description.required_coverage,
                                     .stack_argument_count = description.stack_argument_count});
        return FUNCHOOK_ERROR_SUCCESS;
    }
    catch (...) {
        funchook->error = "could not prepare permanent Windows IAT allocation hook target";
        return FUNCHOOK_ERROR_INTERNAL;
    }
}

extern "C" int funchook_install(funchook_t *funchook, int flags)
{
    (void)flags;
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    if (funchook->installed) {
        return FUNCHOOK_ERROR_SUCCESS;
    }
    try {
        funchook->error.clear();
        if (funchook->records.empty()) {
            funchook->error = "permanent Windows IAT allocation hook target list is empty";
            return FUNCHOOK_ERROR_INTERNAL;
        }
        if (!acquireGateways(funchook->records, funchook->error) ||
            !normalizePreviousGatewaySlots(funchook->records, funchook->error)) {
            return FUNCHOOK_ERROR_INTERNAL;
        }

        auto backend = spark::makeNativeWindowsIatHookBackend(reinterpret_cast<void *>(&funchook_install));
        if (backend == nullptr) {
            funchook->error = "native Windows IAT hook backend is unavailable";
            return FUNCHOOK_ERROR_INTERNAL;
        }
        funchook->hooks = std::make_unique<spark::WindowsIatHooks>(std::move(backend));
        std::vector<spark::WindowsIatHookTarget> targets = makeTargets(funchook->records);
        if (!funchook->hooks->configure(std::move(targets), funchook->error) ||
            !funchook->hooks->install(funchook->error)) {
            funchook->hooks.reset();
            return FUNCHOOK_ERROR_INTERNAL;
        }

        for (HookRecord &record : funchook->records) {
            if (!spark::permanent_iat_gateway_experiment::bindPermanentIatGateway(
                    record.gateway, record.handler, kGatewayDrainTimeoutMs, funchook->error)) {
                std::string detach_error;
                (void)detachGateways(funchook->records, detach_error);
                std::string uninstall_error;
                (void)funchook->hooks->uninstall(uninstall_error);
                funchook->hooks.reset();
                if (!detach_error.empty()) {
                    funchook->error += "; gateway rollback: " + detach_error;
                }
                if (!uninstall_error.empty()) {
                    funchook->error += "; IAT rollback: " + uninstall_error;
                }
                return FUNCHOOK_ERROR_INTERNAL;
            }
        }

        funchook->installed = true;
        return FUNCHOOK_ERROR_SUCCESS;
    }
    catch (const std::exception &exception) {
        funchook->error = std::string("permanent Windows IAT allocation hook install failed: ") + exception.what();
        return FUNCHOOK_ERROR_INTERNAL;
    }
    catch (...) {
        funchook->error = "permanent Windows IAT allocation hook install failed";
        return FUNCHOOK_ERROR_INTERNAL;
    }
}

extern "C" int funchook_refresh(funchook_t *funchook)
{
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    if (!funchook->installed || funchook->hooks == nullptr) {
        return FUNCHOOK_ERROR_NOT_INSTALLED;
    }
    try {
        funchook->error.clear();
        if (!funchook->hooks->refresh(funchook->error)) {
            return FUNCHOOK_ERROR_INTERNAL;
        }
        return FUNCHOOK_ERROR_SUCCESS;
    }
    catch (const std::exception &exception) {
        funchook->error = std::string("permanent Windows IAT allocation hook refresh failed: ") + exception.what();
        return FUNCHOOK_ERROR_INTERNAL;
    }
    catch (...) {
        funchook->error = "permanent Windows IAT allocation hook refresh failed";
        return FUNCHOOK_ERROR_INTERNAL;
    }
}

extern "C" int funchook_uninstall(funchook_t *funchook, int flags)
{
    (void)flags;
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    if (!funchook->installed) {
        return FUNCHOOK_ERROR_NOT_INSTALLED;
    }

    funchook->error.clear();
    if (!detachGateways(funchook->records, funchook->error)) {
        return FUNCHOOK_ERROR_INTERNAL;
    }

    // After every permanent gateway is closed, drained through permanent code,
    // and has cleared its unloadable handler pointer, IAT restoration is only a
    // cleanliness operation. A stale/cached slot that still points at the
    // process-lifetime gateway can only execute the original allocator path.
    if (funchook->hooks != nullptr) {
        std::string detach_error;
        if (!funchook->hooks->uninstall(detach_error) && funchook->error.empty()) {
            funchook->error =
                "Windows allocation IAT detach left safe permanent gateway entries active: " + detach_error;
        }
    }
    funchook->installed = false;
    return FUNCHOOK_ERROR_SUCCESS;
}

extern "C" int funchook_destroy(funchook_t *funchook)
{
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_SUCCESS;
    }
    if (funchook->installed) {
        funchook->error = "permanent Windows allocation hook context is still installed";
        return FUNCHOOK_ERROR_INTERNAL;
    }
    delete funchook;
    return FUNCHOOK_ERROR_SUCCESS;
}

extern "C" const char *funchook_error_message(funchook_t *funchook)
{
    return funchook == nullptr ? "permanent Windows IAT allocation hook context is null" : funchook->error.c_str();
}

extern "C" const char *funchook_backend_id(void)
{
    return "native-ucrt/permanent-iat";
}

extern "C" const char *funchook_backend_name(void)
{
    return "Windows UCRT/permanent IAT gateway";
}
