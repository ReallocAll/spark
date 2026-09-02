#include <funchook.h>

#ifndef _WIN32
#error "windows_allocation_gateway_funchook_compat.cpp must only be compiled on Windows"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
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

#include "native/alloc/windows_permanent_gateway_experiment.h"

namespace {

constexpr std::uint64_t KGatewayDrainTimeoutMs = 5000;

using spark::permanent_gateway_experiment::PermanentGateway;

struct TargetDescription {
    const char *name = nullptr;
    std::uint32_t stack_arguments = 0;
};

struct HookRecord {
    const char *name = nullptr;
    void *entry = nullptr;
    void *handler = nullptr;
    PermanentGateway gateway;
    bool attached = false;
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
        std::uint32_t stack_arguments;
    } candidates[] = {
        {.module = ucrt, .name = "malloc", .stack_arguments = 0},
        {.module = ucrt, .name = "calloc", .stack_arguments = 0},
        {.module = ucrt, .name = "realloc", .stack_arguments = 0},
        {.module = ucrt, .name = "_recalloc", .stack_arguments = 0},
        {.module = ucrt, .name = "free", .stack_arguments = 0},
        {.module = ucrt, .name = "_aligned_malloc", .stack_arguments = 0},
        {.module = ucrt, .name = "_aligned_realloc", .stack_arguments = 0},
        {.module = ucrt, .name = "_aligned_recalloc", .stack_arguments = 0},
        {.module = ucrt, .name = "_aligned_offset_malloc", .stack_arguments = 0},
        // Preserve one stack slot for the offset family. This is harmless for
        // the four-register-argument realloc ABI and is required by recalloc's
        // fifth argument; it also matches the real-target gateway probe.
        {.module = ucrt, .name = "_aligned_offset_realloc", .stack_arguments = 1},
        {.module = ucrt, .name = "_aligned_offset_recalloc", .stack_arguments = 1},
        {.module = ucrt, .name = "_aligned_free", .stack_arguments = 0},
        {.module = ucrt, .name = "_malloc_base", .stack_arguments = 0},
        {.module = ucrt, .name = "_calloc_base", .stack_arguments = 0},
        {.module = ucrt, .name = "_realloc_base", .stack_arguments = 0},
        {.module = ucrt, .name = "_free_base", .stack_arguments = 0},
        {.module = kernel32, .name = "HeapAlloc", .stack_arguments = 0},
        {.module = kernel32, .name = "HeapReAlloc", .stack_arguments = 0},
        {.module = kernel32, .name = "HeapFree", .stack_arguments = 0},
    };
    for (const Candidate &candidate : candidates) {
        if (sameExport(candidate.module, candidate.name, address)) {
            description = {.name = candidate.name, .stack_arguments = candidate.stack_arguments};
            return true;
        }
    }
    return false;
}

std::uint64_t ownerCookie(const void *context) noexcept
{
    const std::uint64_t address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(context));
    const std::uint64_t mixed = address ^ 0xA110CA7E5A7E0001ULL;
    return mixed != 0 ? mixed : 1;
}

}  // namespace

struct funchook {
    std::vector<HookRecord> records;
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
    if (funchook == nullptr || target_func == nullptr || *target_func == nullptr || hook_func == nullptr ||
        funchook->installed) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    try {
        funchook->error.clear();
        TargetDescription description;
        void *entry = *target_func;
        if (!describeTarget(entry, description)) {
            funchook->error = "unsupported Windows permanent-gateway allocation target";
            return FUNCHOOK_ERROR_INTERNAL;
        }
        if (std::ranges::any_of(funchook->records,
                                [entry](const HookRecord &record) { return record.entry == entry; })) {
            funchook->error = "duplicate Windows permanent-gateway allocation target";
            return FUNCHOOK_ERROR_INTERNAL;
        }

        PermanentGateway gateway;
        bool created = false;
        std::string gateway_error;
        if (!PermanentGateway::installOrRediscover(entry, description.stack_arguments, gateway, created, gateway_error)) {
            funchook->error = std::string("permanent gateway prepare failed for ") + description.name + ": " +
                              gateway_error;
            return FUNCHOOK_ERROR_INTERNAL;
        }
        void *trampoline = gateway.originalTrampoline();
        if (trampoline == nullptr) {
            funchook->error = std::string("permanent gateway has no original trampoline for ") + description.name;
            return FUNCHOOK_ERROR_INTERNAL;
        }

        // Publication happens closed/pass-through. Only the later install call
        // exposes the unloadable Spark handler. If a subsequent optional target
        // cannot be prepared, this gateway remains a safe process-lifetime
        // pass-through entry and can be rediscovered by a future reload.
        funchook->records.push_back({.name = description.name,
                                     .entry = entry,
                                     .handler = hook_func,
                                     .gateway = gateway,
                                     .attached = false});
        *target_func = trampoline;
        return FUNCHOOK_ERROR_SUCCESS;
    }
    catch (const std::exception &exception) {
        funchook->error = std::string("Windows permanent-gateway prepare failed: ") + exception.what();
        return FUNCHOOK_ERROR_INTERNAL;
    }
    catch (...) {
        funchook->error = "Windows permanent-gateway prepare failed";
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
            funchook->error = "Windows permanent-gateway target list is empty";
            return FUNCHOOK_ERROR_INTERNAL;
        }
        const std::uint64_t cookie = ownerCookie(funchook);
        std::size_t attached = 0;
        for (; attached < funchook->records.size(); ++attached) {
            HookRecord &record = funchook->records[attached];
            std::string gateway_error;
            if (!record.gateway.attach(record.handler, cookie, gateway_error)) {
                funchook->error = std::string("permanent gateway attach failed for ") + record.name + ": " +
                                  gateway_error;
                break;
            }
            record.attached = true;
        }
        if (attached != funchook->records.size()) {
            for (std::size_t index = attached; index != 0; --index) {
                HookRecord &record = funchook->records[index - 1];
                if (!record.attached) {
                    continue;
                }
                std::string rollback_error;
                if (!record.gateway.detach(cookie, KGatewayDrainTimeoutMs, rollback_error)) {
                    funchook->error += std::string("; rollback detach failed for ") + record.name + ": " +
                                      rollback_error;
                    continue;
                }
                record.attached = false;
            }
            return FUNCHOOK_ERROR_INTERNAL;
        }
        funchook->installed = true;
        return FUNCHOOK_ERROR_SUCCESS;
    }
    catch (const std::exception &exception) {
        funchook->error = std::string("Windows permanent-gateway install failed: ") + exception.what();
        return FUNCHOOK_ERROR_INTERNAL;
    }
    catch (...) {
        funchook->error = "Windows permanent-gateway install failed";
        return FUNCHOOK_ERROR_INTERNAL;
    }
}

extern "C" int funchook_refresh(funchook_t *funchook)
{
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_INTERNAL;
    }
    if (!funchook->installed) {
        return FUNCHOOK_ERROR_NOT_INSTALLED;
    }
    // The allocator export entry itself is permanently patched. Existing IAT
    // slots and imports resolved by modules loaded later all converge on that
    // same entry, so there is no module/IAT refresh operation to perform.
    return FUNCHOOK_ERROR_SUCCESS;
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
    const std::uint64_t cookie = ownerCookie(funchook);
    bool all_detached = true;
    for (std::size_t index = funchook->records.size(); index != 0; --index) {
        HookRecord &record = funchook->records[index - 1];
        if (!record.attached) {
            continue;
        }
        std::string gateway_error;
        if (!record.gateway.detach(cookie, KGatewayDrainTimeoutMs, gateway_error)) {
            if (funchook->error.empty()) {
                funchook->error = std::string("permanent gateway detach failed for ") + record.name + ": " +
                                  gateway_error;
            }
            all_detached = false;
            continue;
        }
        record.attached = false;
    }
    if (!all_detached) {
        // Keep installed=true so shutdown can retry. The sampler treats a
        // persistent detach failure as unload-unsafe and aborts rather than
        // allowing endstone_spark.dll to disappear with a live handler pointer.
        return FUNCHOOK_ERROR_INTERNAL;
    }
    funchook->installed = false;
    return FUNCHOOK_ERROR_SUCCESS;
}

extern "C" int funchook_destroy(funchook_t *funchook)
{
    if (funchook == nullptr) {
        return FUNCHOOK_ERROR_SUCCESS;
    }
    if (funchook->installed ||
        std::ranges::any_of(funchook->records, [](const HookRecord &record) { return record.attached; })) {
        funchook->error = "Windows permanent-gateway hook context is still attached";
        return FUNCHOOK_ERROR_INTERNAL;
    }
    for (const HookRecord &record : funchook->records) {
        if (!record.gateway.valid() || !record.gateway.drained() || record.gateway.handlerAddress() != nullptr) {
            funchook->error = std::string("Windows permanent gateway is not unload-safe for ") + record.name;
            return FUNCHOOK_ERROR_INTERNAL;
        }
    }
    delete funchook;
    return FUNCHOOK_ERROR_SUCCESS;
}

extern "C" const char *funchook_error_message(funchook_t *funchook)
{
    return funchook == nullptr ? "Windows permanent-gateway hook context is null" : funchook->error.c_str();
}
