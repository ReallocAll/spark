#include "application/command/profiler_action_resolver.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace spark {
namespace {

bool isAction(const Arguments &args, const std::string &name)
{
    if (args.boolFlag(name)) {
        return true;
    }
    std::string action = args.subCommand();
    std::ranges::transform(action, action.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return action == name;
}

}  // namespace

ProfilerAction resolveProfilerAction(const Arguments &args)
{
    if (isAction(args, "info")) {
        return ProfilerAction::Info;
    }
    if (isAction(args, "open")) {
        return ProfilerAction::Open;
    }
    if (isAction(args, "trust-viewer")) {
        return ProfilerAction::TrustViewer;
    }
    if (isAction(args, "cancel")) {
        return ProfilerAction::Cancel;
    }
    if (isAction(args, "stop") || isAction(args, "upload")) {
        return ProfilerAction::Stop;
    }
    if (isAction(args, "start")) {
        return ProfilerAction::Start;
    }
    return args.raw().empty() ? ProfilerAction::Info : ProfilerAction::Start;
}

}  // namespace spark
