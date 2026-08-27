#include "core/config/spark_config_environment.h"

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace spark {

namespace {

std::optional<std::string> readEnvironmentValue(const char *name)
{
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

bool isAsciiTrue(std::string_view value)
{
    constexpr std::string_view k_true = "true";
    if (value.size() != k_true.size()) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char lower = value[i] >= 'A' && value[i] <= 'Z' ? static_cast<char>(value[i] - 'A' + 'a') : value[i];
        if (lower != k_true[i]) {
            return false;
        }
    }
    return true;
}

std::optional<bool> readBoolean(const char *name)
{
    const auto value = readEnvironmentValue(name);
    if (!value) {
        return std::nullopt;
    }
    return isAsciiTrue(*value);
}

std::optional<std::int64_t> parseInteger(std::string_view value)
{
    if (value.empty()) {
        return std::nullopt;
    }

    bool positive_sign = false;
    if (value.front() == '+') {
        positive_sign = true;
        value.remove_prefix(1);
        if (value.empty()) {
            return std::nullopt;
        }
    }

    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    if (positive_sign && parsed < 0) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::int64_t> readInteger(const char *name)
{
    const auto value = readEnvironmentValue(name);
    if (!value) {
        return std::nullopt;
    }
    return parseInteger(*value);
}

}  // namespace

SparkConfigEnvironment readSparkConfigEnvironment()
{
    return {.viewer_url = readEnvironmentValue("SPARK_VIEWERURL"),
            .bytebin_url = readEnvironmentValue("SPARK_BYTEBINURL"),
            .bytesocks_host = readEnvironmentValue("SPARK_BYTESOCKSHOST"),
            .background_profiler_enabled = readBoolean("SPARK_BACKGROUNDPROFILER"),
            .background_profiler_interval = readInteger("SPARK_BACKGROUNDPROFILERINTERVAL"),
            .background_profiler_thread_grouper = readEnvironmentValue("SPARK_BACKGROUNDPROFILERTHREADGROUPER"),
            .background_profiler_thread_dumper = readEnvironmentValue("SPARK_BACKGROUNDPROFILERTHREADDUMPER"),
            .allocation_rate_metrics_enabled = readBoolean("SPARK_ALLOCATIONRATEMETRICS"),
            .disable_response_broadcast = readBoolean("SPARK_DISABLERESPONSEBROADCAST")};
}

}  // namespace spark
