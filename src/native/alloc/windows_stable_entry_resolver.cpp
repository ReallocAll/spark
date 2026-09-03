#include "native/alloc/windows_stable_entry_experiment.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstring>
#include <sstream>
#include <string_view>

namespace spark::stable_entry_experiment {
namespace {

struct LogicalTarget {
    const char *module;
    const char *name;
};

constexpr std::array<LogicalTarget, 19> kTargets{{
    {"ucrtbase.dll", "malloc"},
    {"ucrtbase.dll", "calloc"},
    {"ucrtbase.dll", "realloc"},
    {"ucrtbase.dll", "_recalloc"},
    {"ucrtbase.dll", "free"},
    {"ucrtbase.dll", "_aligned_malloc"},
    {"ucrtbase.dll", "_aligned_realloc"},
    {"ucrtbase.dll", "_aligned_recalloc"},
    {"ucrtbase.dll", "_aligned_offset_malloc"},
    {"ucrtbase.dll", "_aligned_offset_realloc"},
    {"ucrtbase.dll", "_aligned_offset_recalloc"},
    {"ucrtbase.dll", "_aligned_free"},
    {"ucrtbase.dll", "_malloc_base"},
    {"ucrtbase.dll", "_calloc_base"},
    {"ucrtbase.dll", "_realloc_base"},
    {"ucrtbase.dll", "_free_base"},
    {"kernel32.dll", "HeapAlloc"},
    {"kernel32.dll", "HeapReAlloc"},
    {"kernel32.dll", "HeapFree"},
}};

std::string windowsError(const char *prefix)
{
    std::ostringstream stream;
    stream << prefix << " (GetLastError=" << ::GetLastError() << ')';
    return stream.str();
}

std::string modulePath(HMODULE module)
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    const int bytes =
        ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length), result.data(), bytes, nullptr,
                              nullptr) != bytes) {
        return {};
    }
    return result;
}

bool owningModule(const void *address, HMODULE &owner, std::string &error)
{
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(address, &info, sizeof(info)) == 0 || info.AllocationBase == nullptr) {
        error = windowsError("VirtualQuery failed while resolving stable entry owner");
        return false;
    }
    owner = static_cast<HMODULE>(info.AllocationBase);
    if (modulePath(owner).empty()) {
        error = "resolved stable allocator entry has no identifiable owning module";
        return false;
    }
    return true;
}

}  // namespace

bool resolveWindowsAllocatorTargets(std::vector<TargetRecord> &targets, bool &dynamic_code_allowed, std::string &error)
{
    targets.clear();
    dynamic_code_allowed = false;
    error.clear();

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy{};
    if (!::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &policy,
                                      static_cast<SIZE_T>(sizeof(policy)))) {
        error = windowsError("GetProcessMitigationPolicy(ProcessDynamicCodePolicy) failed");
        return false;
    }
    dynamic_code_allowed = policy.ProhibitDynamicCode == 0;

    targets.reserve(kTargets.size());
    for (const LogicalTarget &logical : kTargets) {
        HMODULE requested = ::GetModuleHandleA(logical.module);
        bool release_requested = false;
        if (requested == nullptr) {
            requested = ::LoadLibraryA(logical.module);
            release_requested = requested != nullptr;
        }
        if (requested == nullptr) {
            error = windowsError("failed to load requested allocator module");
            return false;
        }

        FARPROC entry = ::GetProcAddress(requested, logical.name);
        if (entry == nullptr) {
            if (release_requested) {
                ::FreeLibrary(requested);
            }
            error = std::string("GetProcAddress failed for ") + logical.module + '!' + logical.name;
            return false;
        }

        HMODULE owner = nullptr;
        if (!owningModule(reinterpret_cast<const void *>(entry), owner, error)) {
            if (release_requested) {
                ::FreeLibrary(requested);
            }
            return false;
        }

        TargetRecord record;
        record.logical_name = logical.name;
        record.requested_module = logical.module;
        record.resolved_address = reinterpret_cast<std::uintptr_t>(entry);
        record.resolved_owner = modulePath(owner);
        if (record.resolved_owner.empty()) {
            if (release_requested) {
                ::FreeLibrary(requested);
            }
            error = "failed to render resolved allocator owner module path";
            return false;
        }
        std::memcpy(record.original_bytes.data(), reinterpret_cast<const void *>(entry), record.original_bytes.size());
        record.original_hash = hashBytes(record.original_bytes.data(), record.original_bytes.size());
        record.patch_window = {record.resolved_address, record.resolved_address + record.original_bytes.size()};
        targets.push_back(std::move(record));

        if (release_requested) {
            ::FreeLibrary(requested);
        }
    }
    return true;
}

}  // namespace spark::stable_entry_experiment

#endif
