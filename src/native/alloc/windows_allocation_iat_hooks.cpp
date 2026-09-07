#include "native/alloc/windows_allocation_iat_hooks.h"

#ifndef _WIN32
#error "windows_allocation_iat_hooks.cpp must only be compiled on Windows"
#endif

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
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
#include "native/alloc/windows_permanent_iat_gateway_registry.h"

namespace spark {
namespace {

constexpr std::uint64_t KGatewayDrainTimeoutMs = 5000;

struct HookRecord {
    const char *name = nullptr;
    void *original = nullptr;
    void *handler = nullptr;
    bool required_coverage = false;
    std::uint32_t stack_argument_count = 0;
    permanent_iat_gateway::PermanentIatGatewayHandle gateway;
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
        {ucrt, "malloc", true, 0},
        {ucrt, "calloc", false, 0},
        {ucrt, "realloc", false, 0},
        {ucrt, "_recalloc", false, 0},
        {ucrt, "free", true, 0},
        {ucrt, "_aligned_malloc", false, 0},
        {ucrt, "_aligned_realloc", false, 0},
        {ucrt, "_aligned_recalloc", false, 0},
        {ucrt, "_aligned_offset_malloc", false, 0},
        {ucrt, "_aligned_offset_realloc", false, 0},
        {ucrt, "_aligned_offset_recalloc", false, 1},
        {ucrt, "_aligned_free", false, 0},
        {ucrt, "_malloc_base", false, 0},
        {ucrt, "_calloc_base", false, 0},
        {ucrt, "_realloc_base", false, 0},
        {ucrt, "_free_base", false, 0},
        {kernel32, "HeapAlloc", false, 0},
        {kernel32, "HeapReAlloc", false, 0},
        {kernel32, "HeapFree", false, 0},
    };
    for (const Candidate &candidate : candidates) {
        if (sameExport(candidate.module, candidate.name, address)) {
            description = {candidate.name, candidate.required_coverage, candidate.stack_argument_count};
            return true;
        }
    }
    return false;
}

std::vector<WindowsIatHookTarget> makeTargets(const std::vector<HookRecord> &records)
{
    std::vector<WindowsIatHookTarget> targets;
    targets.reserve(records.size());
    for (const HookRecord &record : records) {
        targets.push_back({record.name, {}, record.original, record.gateway.gateway, record.required_coverage});
    }
    return targets;
}

void backendAnchor() noexcept {}

bool acquireGateways(std::vector<HookRecord> &records, std::string &error)
{
    for (HookRecord &record : records) {
        if (!permanent_iat_gateway::acquirePermanentIatGateway(record.original, record.stack_argument_count,
                                                                record.gateway, error)) {
            return false;
        }
        if (permanent_iat_gateway::permanentIatGatewayAdmissionOpen(record.gateway) ||
            permanent_iat_gateway::permanentIatGatewayHandler(record.gateway) != nullptr) {
            error = std::string("permanent IAT gateway is still bound from an earlier Spark image: ") + record.name;
            return false;
        }
    }
    return true;
}

bool normalizePreviousGatewaySlots(const std::vector<HookRecord> &records, std::string &error)
{
    auto backend = makeNativeWindowsIatHookBackend(reinterpret_cast<void *>(&backendAnchor));
    if (backend == nullptr) {
        error = "native Windows IAT hook backend is unavailable during permanent-gateway normalization";
        return false;
    }
    const std::vector<WindowsIatHookTarget> targets = makeTargets(records);
    std::vector<WindowsIatSlot> slots;
    if (!backend->enumerate(targets, slots, error)) {
        return false;
    }
    for (const WindowsIatSlot &slot : slots) {
        if (slot.target_index >= targets.size()) {
            error = "native Windows IAT backend returned an invalid target index during gateway normalization";
            return false;
        }
        const WindowsIatHookTarget &target = targets[slot.target_index];
        void *current = nullptr;
        std::string access_error;
        const WindowsIatAccessStatus read_status = backend->read(slot, current, access_error);
        if (read_status == WindowsIatAccessStatus::Stale) {
            continue;
        }
        if (read_status == WindowsIatAccessStatus::Error) {
            error = access_error.empty() ? "failed to inspect a previous permanent IAT gateway slot" : access_error;
            return false;
        }
        if (current != target.replacement) {
            continue;
        }
        std::string exchange_error;
        const WindowsIatExchangeResult exchange =
            backend->compareExchange(slot, target.replacement, target.original, exchange_error);
        if (exchange.status == WindowsIatExchangeStatus::Exchanged ||
            exchange.status == WindowsIatExchangeStatus::Stale ||
            (exchange.status == WindowsIatExchangeStatus::Mismatch && exchange.observed != target.replacement)) {
            continue;
        }
        current = nullptr;
        access_error.clear();
        const WindowsIatAccessStatus after_status = backend->read(slot, current, access_error);
        if (after_status == WindowsIatAccessStatus::Stale ||
            (after_status == WindowsIatAccessStatus::Accessible && current != target.replacement)) {
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
        if (!permanent_iat_gateway::permanentIatGatewayAdmissionOpen(record.gateway) &&
            permanent_iat_gateway::permanentIatGatewayHandler(record.gateway) == nullptr) {
            continue;
        }
        std::string detach_error;
        if (!permanent_iat_gateway::detachPermanentIatGateway(record.gateway, KGatewayDrainTimeoutMs, detach_error)) {
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

struct WindowsAllocationIatHooks::Impl {
    std::vector<HookRecord> records;
    std::unique_ptr<WindowsIatHooks> hooks;
    bool installed = false;
    std::string error;
};

WindowsAllocationIatHooks::WindowsAllocationIatHooks() : impl_(std::make_unique<Impl>()) {}
WindowsAllocationIatHooks::~WindowsAllocationIatHooks() = default;

bool WindowsAllocationIatHooks::addTarget(void *target, void *handler, std::string &error)
{
    error.clear();
    if (target == nullptr || handler == nullptr) {
        error = "Windows allocation IAT target or handler is null";
        return false;
    }
    try {
        TargetDescription description;
        if (!describeTarget(target, description)) {
            error = "unsupported Windows allocation hook target";
            impl_->error = error;
            return false;
        }
        impl_->records.push_back({
            .name = description.name,
            .original = target,
            .handler = handler,
            .required_coverage = description.required_coverage,
            .stack_argument_count = description.stack_argument_count,
            .gateway = {},
        });
        return true;
    }
    catch (...) {
        error = "could not prepare permanent Windows IAT allocation hook target";
        impl_->error = error;
        return false;
    }
}

bool WindowsAllocationIatHooks::install(std::string &error)
{
    error.clear();
    if (impl_->installed) {
        return true;
    }
    try {
        impl_->error.clear();
        if (impl_->records.empty()) {
            error = "permanent Windows IAT allocation hook target list is empty";
            impl_->error = error;
            return false;
        }
        if (!acquireGateways(impl_->records, error) || !normalizePreviousGatewaySlots(impl_->records, error)) {
            impl_->error = error;
            return false;
        }
        auto backend = makeNativeWindowsIatHookBackend(reinterpret_cast<void *>(&backendAnchor));
        if (backend == nullptr) {
            error = "native Windows IAT hook backend is unavailable";
            impl_->error = error;
            return false;
        }
        impl_->hooks = std::make_unique<WindowsIatHooks>(std::move(backend));
        if (!impl_->hooks->configure(makeTargets(impl_->records), error) || !impl_->hooks->install(error)) {
            impl_->hooks.reset();
            impl_->error = error;
            return false;
        }
        for (HookRecord &record : impl_->records) {
            if (!permanent_iat_gateway::bindPermanentIatGateway(record.gateway, record.handler, KGatewayDrainTimeoutMs,
                                                                 error)) {
                std::string detach_error;
                (void)detachGateways(impl_->records, detach_error);
                std::string uninstall_error;
                (void)impl_->hooks->uninstall(uninstall_error);
                impl_->hooks.reset();
                if (!detach_error.empty()) {
                    error += "; gateway rollback: " + detach_error;
                }
                if (!uninstall_error.empty()) {
                    error += "; IAT rollback: " + uninstall_error;
                }
                impl_->error = error;
                return false;
            }
        }
        impl_->installed = true;
        return true;
    }
    catch (const std::exception &exception) {
        error = std::string("permanent Windows IAT allocation hook install failed: ") + exception.what();
    }
    catch (...) {
        error = "permanent Windows IAT allocation hook install failed";
    }
    impl_->error = error;
    return false;
}

bool WindowsAllocationIatHooks::refresh(std::string &error)
{
    error.clear();
    if (!impl_->installed || impl_->hooks == nullptr) {
        error = "permanent Windows IAT allocation hook context is not installed";
        impl_->error = error;
        return false;
    }
    try {
        if (!impl_->hooks->refresh(error)) {
            impl_->error = error;
            return false;
        }
        return true;
    }
    catch (const std::exception &exception) {
        error = std::string("permanent Windows IAT allocation hook refresh failed: ") + exception.what();
    }
    catch (...) {
        error = "permanent Windows IAT allocation hook refresh failed";
    }
    impl_->error = error;
    return false;
}

bool WindowsAllocationIatHooks::uninstall(std::string &error) noexcept
{
    error.clear();
    if (!impl_->installed) {
        return true;
    }
    if (!detachGateways(impl_->records, error)) {
        impl_->error = error;
        return false;
    }
    if (impl_->hooks != nullptr) {
        std::string detach_error;
        if (!impl_->hooks->uninstall(detach_error) && error.empty()) {
            error = "Windows allocation IAT detach left safe permanent gateway entries active: " + detach_error;
        }
    }
    impl_->installed = false;
    impl_->error = error;
    return true;
}

bool WindowsAllocationIatHooks::installed() const noexcept
{
    return impl_->installed;
}

const std::string &WindowsAllocationIatHooks::lastError() const noexcept
{
    return impl_->error;
}

}  // namespace spark
