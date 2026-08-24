#include <cassert>
#include <initializer_list>
#include <string>
#include <vector>

#include "application/command/profiler_action_resolver.h"

namespace {

spark::ProfilerAction resolve(std::initializer_list<std::string> tokens)
{
    return spark::resolveProfilerAction(spark::Arguments(std::vector<std::string>(tokens), true));
}

}  // namespace

int main()
{
    using spark::ProfilerAction;

    assert(resolve({}) == ProfilerAction::Info);
    assert(resolve({"info"}) == ProfilerAction::Info);
    assert(resolve({"--info"}) == ProfilerAction::Info);
    assert(resolve({"open"}) == ProfilerAction::Open);
    assert(resolve({"--open"}) == ProfilerAction::Open);
    assert(resolve({"trust-viewer"}) == ProfilerAction::TrustViewer);
    assert(resolve({"--trust-viewer"}) == ProfilerAction::TrustViewer);
    assert(resolve({"cancel"}) == ProfilerAction::Cancel);
    assert(resolve({"--cancel"}) == ProfilerAction::Cancel);
    assert(resolve({"stop"}) == ProfilerAction::Stop);
    assert(resolve({"upload"}) == ProfilerAction::Stop);
    assert(resolve({"--stop"}) == ProfilerAction::Stop);
    assert(resolve({"--upload"}) == ProfilerAction::Stop);
    assert(resolve({"start"}) == ProfilerAction::Start);
    assert(resolve({"--start"}) == ProfilerAction::Start);
    assert(resolve({"unknown"}) == ProfilerAction::Start);
    assert(resolve({"--unknown", "value"}) == ProfilerAction::Start);

    assert(resolve({"start", "--info", "--open", "--trust-viewer", "--cancel", "--stop"}) == ProfilerAction::Info);
    assert(resolve({"start", "--open", "--trust-viewer", "--cancel", "--stop"}) == ProfilerAction::Open);
    assert(resolve({"start", "--trust-viewer", "--cancel", "--stop"}) == ProfilerAction::TrustViewer);
    assert(resolve({"start", "--cancel", "--stop"}) == ProfilerAction::Cancel);
    assert(resolve({"start", "--stop"}) == ProfilerAction::Stop);

    return 0;
}
