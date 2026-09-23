#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ll/api/command/ParamTraits.h"
#include "mc/server/commands/Command.h"
#include "mc/server/commands/CommandRawText.h"

namespace {

struct RawParameters final {
    ::CommandRawText raw;
};

struct RawCommandLayout final : ::Command {
    std::uint64_t placeholder = 0;
    RawParameters parameters;

    void execute(::CommandOrigin const &, ::CommandOutput &) const override {}
};

static_assert(std::is_default_constructible_v<RawParameters>);
static_assert(offsetof(RawCommandLayout, parameters) > sizeof(::Command));
static_assert(offsetof(RawCommandLayout, parameters) + offsetof(RawParameters, raw) > sizeof(::Command));
static_assert(ll::command::ParamTraits<::CommandRawText>::parseRuleValue() ==
              ::CommandRegistry::HardNonTerminal::RawText);
static_assert(
    std::is_same_v<decltype(ll::command::ParamTraits<::CommandRawText>::parseFn()), ::CommandRegistry::ParseFunction>);

}  // namespace

int main()
{
    return 0;
}
