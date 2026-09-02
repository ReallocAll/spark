#include "native/alloc/windows_permanent_gateway_experiment.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using spark::permanent_gateway_experiment::PermanentGateway;

struct LogicalTarget {
    const wchar_t *module;
    const char *name;
    std::uint32_t stack_arguments;
};

struct UniqueTarget {
    void *entry = nullptr;
    std::uint32_t stack_arguments = 0;
    PermanentGateway gateway;
    std::vector<const char *> aliases;
};

constexpr std::array<LogicalTarget, 19> kTargets{{
    {L"ucrtbase.dll", "malloc", 0},
    {L"ucrtbase.dll", "calloc", 0},
    {L"ucrtbase.dll", "realloc", 0},
    {L"ucrtbase.dll", "_recalloc", 0},
    {L"ucrtbase.dll", "free", 0},
    {L"ucrtbase.dll", "_aligned_malloc", 0},
    {L"ucrtbase.dll", "_aligned_realloc", 0},
    {L"ucrtbase.dll", "_aligned_recalloc", 0},
    {L"ucrtbase.dll", "_aligned_offset_malloc", 0},
    {L"ucrtbase.dll", "_aligned_offset_realloc", 1},
    {L"ucrtbase.dll", "_aligned_offset_recalloc", 1},
    {L"ucrtbase.dll", "_aligned_free", 0},
    {L"ucrtbase.dll", "_malloc_base", 0},
    {L"ucrtbase.dll", "_calloc_base", 0},
    {L"ucrtbase.dll", "_realloc_base", 0},
    {L"ucrtbase.dll", "_free_base", 0},
    {L"kernel32.dll", "HeapAlloc", 0},
    {L"kernel32.dll", "HeapReAlloc", 0},
    {L"kernel32.dll", "HeapFree", 0},
}};

void printBytes(void *entry) {
    std::array<unsigned char, 16> bytes{};
    std::memcpy(bytes.data(), entry, bytes.size());
    for (unsigned char byte : bytes) {
        std::printf("%02X", static_cast<unsigned>(byte));
    }
}

bool exerciseApis() {
    void *p = std::malloc(1234);
    void *c = std::calloc(7, 333);
    if (p == nullptr || c == nullptr) {
        std::free(p);
        std::free(c);
        return false;
    }
    p = std::realloc(p, 4097);
    if (p == nullptr) {
        std::free(c);
        return false;
    }
    void *r = _recalloc(nullptr, 11, 257);
    void *a = _aligned_malloc(5003, 64);
    void *ao = _aligned_offset_malloc(4099, 64, 7);
    if (r == nullptr || a == nullptr || ao == nullptr) {
        std::free(p);
        std::free(c);
        std::free(r);
        _aligned_free(a);
        _aligned_free(ao);
        return false;
    }
    a = _aligned_realloc(a, 9001, 64);
    if (a == nullptr) {
        std::free(p);
        std::free(c);
        std::free(r);
        _aligned_free(ao);
        return false;
    }
    a = _aligned_recalloc(a, 3, 4096, 64);
    if (a == nullptr) {
        std::free(p);
        std::free(c);
        std::free(r);
        _aligned_free(ao);
        return false;
    }
    ao = _aligned_offset_realloc(ao, 10003, 64, 7);
    if (ao == nullptr) {
        std::free(p);
        std::free(c);
        std::free(r);
        _aligned_free(a);
        return false;
    }
    ao = _aligned_offset_recalloc(ao, 2, 7001, 64, 7);
    if (ao == nullptr) {
        std::free(p);
        std::free(c);
        std::free(r);
        _aligned_free(a);
        return false;
    }

    HANDLE heap = GetProcessHeap();
    void *h = HeapAlloc(heap, 0, 2049);
    if (h == nullptr) {
        std::free(p);
        std::free(c);
        std::free(r);
        _aligned_free(a);
        _aligned_free(ao);
        return false;
    }
    h = HeapReAlloc(heap, 0, h, 8193);
    if (h == nullptr) {
        std::free(p);
        std::free(c);
        std::free(r);
        _aligned_free(a);
        _aligned_free(ao);
        return false;
    }

    std::free(p);
    std::free(c);
    std::free(r);
    _aligned_free(a);
    _aligned_free(ao);
    if (HeapFree(heap, 0, h) == FALSE) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::printf("permanent-gateway-real-targets begin pid=%lu\n", GetCurrentProcessId());

    std::unordered_map<void *, std::size_t> unique_by_entry;
    std::vector<UniqueTarget> unique;
    std::size_t missing_optional = 0;

    for (const LogicalTarget &logical : kTargets) {
        HMODULE module = GetModuleHandleW(logical.module);
        if (module == nullptr) {
            std::fprintf(stderr, "module missing target=%s error=%lu\n", logical.name, GetLastError());
            return 1;
        }
        void *entry = reinterpret_cast<void *>(GetProcAddress(module, logical.name));
        if (entry == nullptr) {
            // Internal UCRT base exports can differ across servicing builds. Record
            // absence, but do not reject the gateway engine solely for an optional
            // logical target that the production sampler already treats optional.
            const bool required = std::strcmp(logical.name, "malloc") == 0 || std::strcmp(logical.name, "calloc") == 0 ||
                                  std::strcmp(logical.name, "realloc") == 0 || std::strcmp(logical.name, "free") == 0 ||
                                  std::strcmp(logical.name, "_aligned_free") == 0 ||
                                  std::strcmp(logical.name, "HeapFree") == 0;
            std::printf("target name=%s status=missing required=%d\n", logical.name, required ? 1 : 0);
            if (required) {
                return 2;
            }
            ++missing_optional;
            continue;
        }

        std::printf("target name=%s entry=%p align8=%d stack_args=%u bytes=", logical.name, entry,
                    (reinterpret_cast<std::uintptr_t>(entry) & 7U) == 0 ? 1 : 0, logical.stack_arguments);
        printBytes(entry);
        std::printf("\n");

        auto found = unique_by_entry.find(entry);
        if (found != unique_by_entry.end()) {
            UniqueTarget &existing = unique[found->second];
            if (existing.stack_arguments != logical.stack_arguments) {
                std::fprintf(stderr,
                             "ABI-conflicting alias entry=%p first=%s second=%s first_stack=%u second_stack=%u\n", entry,
                             existing.aliases.front(), logical.name, existing.stack_arguments, logical.stack_arguments);
                return 3;
            }
            existing.aliases.push_back(logical.name);
            std::printf("target name=%s status=alias canonical=%s\n", logical.name, existing.aliases.front());
            continue;
        }

        PermanentGateway gateway;
        bool created = false;
        std::string error;
        if (!PermanentGateway::installOrRediscover(entry, logical.stack_arguments, gateway, created, error)) {
            std::fprintf(stderr, "install failed target=%s entry=%p error=%s\n", logical.name, entry, error.c_str());
            return 4;
        }
        if (!gateway.valid() || gateway.handlerAddress() != nullptr || !gateway.drained()) {
            std::fprintf(stderr, "closed pass-through invariant failed target=%s\n", logical.name);
            return 5;
        }

        UniqueTarget item;
        item.entry = entry;
        item.stack_arguments = logical.stack_arguments;
        item.gateway = std::move(gateway);
        item.aliases.push_back(logical.name);
        const auto footprint = item.gateway.footprint();
        std::printf(
            "target name=%s status=installed created=%d gateway=%p trampoline=%p island=%zu state=%zu trampoline_mem=%zu\n",
            logical.name, created ? 1 : 0, item.gateway.gatewayEntry(), item.gateway.originalTrampoline(),
            footprint.island_committed, footprint.state_committed, footprint.trampoline_committed);
        unique_by_entry.emplace(entry, unique.size());
        unique.push_back(std::move(item));
    }

    if (!exerciseApis()) {
        std::fprintf(stderr, "allocator API exercise failed after permanent pass-through installation\n");
        return 6;
    }

    std::size_t rediscovered = 0;
    std::size_t total_footprint = 0;
    for (UniqueTarget &item : unique) {
        PermanentGateway second;
        bool created = true;
        std::string error;
        if (!PermanentGateway::installOrRediscover(item.entry, item.stack_arguments, second, created, error)) {
            std::fprintf(stderr, "rediscovery failed entry=%p canonical=%s error=%s\n", item.entry, item.aliases.front(),
                         error.c_str());
            return 7;
        }
        if (created || second.gatewayEntry() != item.gateway.gatewayEntry() ||
            second.originalTrampoline() != item.gateway.originalTrampoline() || second.stateBase() != item.gateway.stateBase()) {
            std::fprintf(stderr, "rediscovery identity mismatch entry=%p canonical=%s\n", item.entry, item.aliases.front());
            return 8;
        }
        const auto footprint = item.gateway.footprint();
        total_footprint += footprint.island_committed + footprint.state_committed + footprint.trampoline_committed;
        ++rediscovered;
    }

    std::printf("permanent-gateway-real-targets PASS logical=%zu unique=%zu missing_optional=%zu rediscovered=%zu footprint=%zu\n",
                kTargets.size(), unique.size(), missing_optional, rediscovered, total_footprint);
    return 0;
}
