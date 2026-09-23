#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/mod/Mod.h"
#include "ll/api/utils/SystemUtils.h"
#include "mc/server/commands/CommandParameterData.h"

namespace spark::levilamina {

enum class HostImageAccess {
    Readable,
    Executable
};

[[nodiscard]] inline bool hostImageProtectionAllowed(std::uint32_t protection, HostImageAccess access) noexcept
{
    constexpr std::uint32_t page_no_access = 0x01U;
    constexpr std::uint32_t page_read_only = 0x02U;
    constexpr std::uint32_t page_read_write = 0x04U;
    constexpr std::uint32_t page_write_copy = 0x08U;
    constexpr std::uint32_t page_execute = 0x10U;
    constexpr std::uint32_t page_execute_read = 0x20U;
    constexpr std::uint32_t page_execute_read_write = 0x40U;
    constexpr std::uint32_t page_execute_write_copy = 0x80U;
    constexpr std::uint32_t page_guard = 0x100U;
    const auto base = protection & 0xffU;
    if ((protection & page_guard) != 0 || base == page_no_access) {
        return false;
    }
    if (access == HostImageAccess::Executable) {
        return base == page_execute || base == page_execute_read || base == page_execute_read_write ||
               base == page_execute_write_copy;
    }
    return base == page_read_only || base == page_read_write || base == page_write_copy || base == page_execute_read ||
           base == page_execute_read_write || base == page_execute_write_copy;
}

struct HostRawParameterTemplate final {
    Bedrock::typeid_t<::CommandRegistry> type_index{};
    ::CommandParameterData::ParseFunction parse_override = nullptr;
    ::CommandRegistry::ParamParseRule const *parse_rule = nullptr;
    std::string rule_module;
    std::string parser_module;
};

[[nodiscard]] inline HostRawParameterTemplate copyHostRawParameterFields(::CommandParameterData const &data,
                                                                         std::string rule_module,
                                                                         std::string parser_module)
{
    return HostRawParameterTemplate{
        .type_index = data.mTypeIndex,
        .parse_override = data.mParseOverride,
        .parse_rule = data.mParseRule,
        .rule_module = std::move(rule_module),
        .parser_module = std::move(parser_module),
    };
}

[[nodiscard]] std::optional<HostRawParameterTemplate> captureHostRawParameterTemplate(
    ::ll::command::CommandHandle &command, std::weak_ptr<ll::mod::Mod> mod, ll::sys_utils::HandleT current_module,
    std::string &error);

[[nodiscard]] bool validateHostRawParameterTemplate(HostRawParameterTemplate &value,
                                                    ll::sys_utils::HandleT current_module, std::string &error);

}  // namespace spark::levilamina
