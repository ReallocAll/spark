#ifndef SPARK_APPLICATION_PROFILER_PROFILER_START_OPTIONS_H
#define SPARK_APPLICATION_PROFILER_PROFILER_START_OPTIONS_H

#include <string>

#include "core/command/arguments.h"
#include "core/profiler/profiler.h"

namespace spark {

struct ProfilerStartOptionsResult {
    ProfilerOptions options;
    std::string error;

    [[nodiscard]] bool success() const { return error.empty(); }
};

ProfilerStartOptionsResult parseProfilerStartOptions(const Arguments &args);

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILER_START_OPTIONS_H
