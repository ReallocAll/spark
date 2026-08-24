#ifndef SPARK_CORE_CONFIG_SPARK_CONFIG_ENVIRONMENT_H
#define SPARK_CORE_CONFIG_SPARK_CONFIG_ENVIRONMENT_H

#include <cstdint>
#include <optional>
#include <string>

namespace spark {

struct SparkConfigEnvironment {
    std::optional<std::string> viewer_url;
    std::optional<std::string> bytebin_url;
    std::optional<std::string> bytesocks_host;
    std::optional<bool> background_profiler_enabled;
    std::optional<std::int64_t> background_profiler_interval;
    std::optional<std::string> background_profiler_thread_grouper;
    std::optional<std::string> background_profiler_thread_dumper;
    std::optional<bool> disable_response_broadcast;
};

SparkConfigEnvironment readSparkConfigEnvironment();

}  // namespace spark

#endif  // SPARK_CORE_CONFIG_SPARK_CONFIG_ENVIRONMENT_H
