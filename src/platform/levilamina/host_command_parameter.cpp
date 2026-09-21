#include "platform/levilamina/host_command_parameter.h"

#include <bit>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ll/api/command/runtime/ParamKind.h"
#include "ll/api/command/runtime/RuntimeOverload.h"
#include "mc/server/commands/CommandRegistry.h"
#include "platform/levilamina/host_parser_provenance.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace spark::levilamina {

namespace {

#ifdef _WIN32

std::string basenameForModule(HMODULE module) noexcept
{
    wchar_t path[32768]{};
    const DWORD length = ::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) {
        return {};
    }
    std::wstring_view value{path, length};
    const auto separator = value.find_last_of(L"\\/");
    value = separator == std::wstring_view::npos ? value : value.substr(separator + 1);
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character > 0x7f) {
            return {};
        }
        const auto ascii = static_cast<char>(character);
        result.push_back(ascii >= 'A' && ascii <= 'Z' ? static_cast<char>(ascii + ('a' - 'A')) : ascii);
    }
    return result;
}

bool isHostModuleName(std::string_view name) noexcept
{
    return name == "levilamina.dll" || name == "bedrock_server.exe" || name == "bedrock_server_mod.exe" ||
           name == "bedrock_runtime.dll";
}

bool moduleForAddress(void const *address, HMODULE &module, std::string &basename) noexcept
{
    module = nullptr;
    if (address == nullptr ||
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<wchar_t const *>(address), &module) == FALSE) {
        return false;
    }
    basename = basenameForModule(module);
    return !basename.empty();
}

bool queryHostImageRange(void const *address, std::size_t size, HMODULE current_module, HostImageAccess access,
                         HMODULE &containing_module, std::string &basename) noexcept
{
    containing_module = nullptr;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin == 0 || size == 0 || size > (std::numeric_limits<std::uintptr_t>::max)() - begin) {
        return false;
    }
    const auto end = begin + size;
    auto cursor = begin;
    HMODULE first_module = nullptr;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<void const *>(cursor), &info, sizeof(info)) != sizeof(info) ||
            info.Type != MEM_IMAGE || info.State != MEM_COMMIT || info.AllocationBase == current_module ||
            (info.Protect & PAGE_GUARD) != 0 || (info.Protect & 0xffU) == PAGE_NOACCESS) {
            return false;
        }
        if (!hostImageProtectionAllowed(static_cast<std::uint32_t>(info.Protect), access)) {
            return false;
        }
        const auto region_begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize == 0 || region_begin > (std::numeric_limits<std::uintptr_t>::max)() - info.RegionSize) {
            return false;
        }
        const auto region_end = region_begin + info.RegionSize;
        if (cursor < region_begin || region_end <= cursor) {
            return false;
        }
        HMODULE module = nullptr;
        std::string current_basename;
        if (!moduleForAddress(reinterpret_cast<void const *>(cursor), module, current_basename) ||
            !isHostModuleName(current_basename) || (first_module != nullptr && module != first_module)) {
            return false;
        }
        if (first_module == nullptr) {
            first_module = module;
            containing_module = module;
            basename = current_basename;
        }
        cursor = region_end < end ? region_end : end;
    }
    return first_module != nullptr;
}

class WindowsBoundedImageReader final : public detail::BoundedImageReader {
public:
    [[nodiscard]] bool query(std::uintptr_t query_address, std::size_t query_size,
                             detail::MemoryRegion &region) const noexcept override
    {
        region = {};
        if (query_address == 0 || query_size == 0 ||
            query_size > (std::numeric_limits<std::uintptr_t>::max)() - query_address) {
            return false;
        }
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<void const *>(query_address), &info, sizeof(info)) != sizeof(info)) {
            return false;
        }
        const auto region_base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize == 0 || region_base > (std::numeric_limits<std::uintptr_t>::max)() - info.RegionSize) {
            return false;
        }
        const auto region_end = region_base + info.RegionSize;
        const auto query_end = query_address + query_size;
        if (query_address < region_base || query_end > region_end) {
            return false;
        }
        region = detail::MemoryRegion{
            .allocation_base = reinterpret_cast<std::uintptr_t>(info.AllocationBase),
            .region_base = region_base,
            .region_size = static_cast<std::uintptr_t>(info.RegionSize),
            .protection = static_cast<std::uint32_t>(info.Protect),
            .state = static_cast<std::uint32_t>(info.State),
            .type = static_cast<std::uint32_t>(info.Type),
        };
        return true;
    }

    [[nodiscard]] bool read(std::uintptr_t read_address, void *destination,
                            std::size_t read_size) const noexcept override
    {
        SIZE_T bytes_read = 0;
        return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<void const *>(read_address), destination,
                                   read_size, &bytes_read) != FALSE &&
               bytes_read == read_size;
    }
};

class WindowsHostRawParameterValidationEnvironment final : public detail::HostRawParameterValidationEnvironment {
public:
    [[nodiscard]] bool imageRangeOwnedByHost(std::uintptr_t address, std::size_t size, std::uintptr_t current_module,
                                             detail::HostRawImageAccess access, std::uintptr_t &containing_module,
                                             std::string &basename) const noexcept override
    {
        HMODULE containing = nullptr;
        const bool result = queryHostImageRange(
            reinterpret_cast<void const *>(address), size, reinterpret_cast<HMODULE>(current_module),
            access == detail::HostRawImageAccess::Readable ? HostImageAccess::Readable : HostImageAccess::Executable,
            containing, basename);
        containing_module = reinterpret_cast<std::uintptr_t>(containing);
        return result;
    }

    [[nodiscard]] std::uintptr_t loadedLeviLamina() const noexcept override
    {
        return reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(L"LeviLamina.dll"));
    }

    [[nodiscard]] detail::BoundedImageReader const &reader() const noexcept override { return reader_; }

private:
    WindowsBoundedImageReader reader_;
};

#endif

}  // namespace

bool validateHostRawParameterTemplate(HostRawParameterTemplate &value, ll::sys_utils::HandleT current_module,
                                      std::string &error)
{
#ifndef _WIN32
    static_cast<void>(value);
    static_cast<void>(current_module);
    error = "host raw parameter validation requires Windows image metadata";
    return false;
#else
    HMODULE current = reinterpret_cast<HMODULE>(current_module);
    WindowsHostRawParameterValidationEnvironment environment;
    return detail::validateHostRawParameterTemplateInternal(value, reinterpret_cast<std::uintptr_t>(current),
                                                            environment, error);
#endif
}

std::optional<HostRawParameterTemplate> captureHostRawParameterTemplate(::ll::command::CommandHandle &command,
                                                                        std::weak_ptr<ll::mod::Mod> mod,
                                                                        ll::sys_utils::HandleT current_module,
                                                                        std::string &error)
{
#ifndef _WIN32
    static_cast<void>(command);
    static_cast<void>(mod);
    static_cast<void>(current_module);
    error = "host raw parameter validation requires Windows image metadata";
    return std::nullopt;
#else
    if (current_module == nullptr) {
        error = "current Spark module handle is unavailable";
        return std::nullopt;
    }
    try {
        auto runtime = command.runtimeOverload(std::move(mod));
        std::optional<HostRawParameterTemplate> result;
        auto &runtime_with_raw = runtime.required("raw", ll::command::ParamKind::RawText);
        static_cast<void>(runtime_with_raw.modify([&](::CommandParameterData &data) {
            if (result.has_value()) {
                throw std::runtime_error{"host raw parameter template was duplicated"};
            }
            if (data.mName != "raw" || data.mParamType != ::CommandParameterDataType::Basic || data.mIsOptional ||
                data.mParseRule == nullptr) {
                throw std::runtime_error{"host raw parameter template has unexpected metadata"};
            }

            std::string rule_module;
            std::string parser_module;
            result = copyHostRawParameterFields(data, std::move(rule_module), std::move(parser_module));
            if (!validateHostRawParameterTemplate(*result, current_module, error)) {
                throw std::runtime_error{error};
            }
        }));
        if (!result.has_value()) {
            error = "host raw parameter template was not produced";
            return std::nullopt;
        }
        return result;
    }
    catch (std::exception const &exception) {
        error = exception.what();
        return std::nullopt;
    }
    catch (...) {
        error = "unknown host raw parameter template failure";
        return std::nullopt;
    }
#endif
}

}  // namespace spark::levilamina
