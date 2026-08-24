#ifndef SPARK_APPLICATION_COMMAND_PROFILER_ACTION_RESOLVER_H
#define SPARK_APPLICATION_COMMAND_PROFILER_ACTION_RESOLVER_H

#include "core/command/arguments.h"

namespace spark {

enum class ProfilerAction {
    Info,
    Open,
    TrustViewer,
    Cancel,
    Stop,
    Start,
};

ProfilerAction resolveProfilerAction(const Arguments &args);

}  // namespace spark

#endif  // SPARK_APPLICATION_COMMAND_PROFILER_ACTION_RESOLVER_H
