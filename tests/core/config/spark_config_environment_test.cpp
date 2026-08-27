#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/config/spark_config.h"
#include "spark_config_test_cases.h"
#include "spark_config_test_support.h"

namespace spark::config_test {

namespace {

constexpr std::array<const char *, 10> KEnvironmentNames = {"SPARK_VIEWERURL",
                                                            "SPARK_BYTEBINURL",
                                                            "SPARK_BYTESOCKSHOST",
                                                            "SPARK_BACKGROUNDPROFILER",
                                                            "SPARK_BACKGROUNDPROFILERINTERVAL",
                                                            "SPARK_BACKGROUNDPROFILERTHREADGROUPER",
                                                            "SPARK_BACKGROUNDPROFILERTHREADDUMPER",
                                                            "SPARK_ALLOCATIONRATEMETRICS",
                                                            "SPARK_DISABLERESPONSEBROADCAST",
                                                            "SPARK_VIEWER_URL"};

void setEnvironment(const char *name, std::string_view value)
{
#ifdef _WIN32
    assert(_putenv_s(name, std::string(value).c_str()) == 0);
#else
    assert(::setenv(name, std::string(value).c_str(), 1) == 0);
#endif
}

void clearEnvironment(const char *name)
{
#ifdef _WIN32
    assert(_putenv_s(name, "") == 0);
#else
    assert(::unsetenv(name) == 0);
#endif
}

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const std::array<const char *, 10> &names)
    {
        for (const char *name : names) {
            const char *value = std::getenv(name);
            previous_.emplace_back(name, value == nullptr ? std::nullopt : std::optional<std::string>(value));
            clearEnvironment(name);
        }
    }

    ~ScopedEnvironment()
    {
        for (const auto &[name, value] : previous_) {
            if (value) {
                setEnvironment(name.c_str(), *value);
            }
            else {
                clearEnvironment(name.c_str());
            }
        }
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> previous_;
};

void writeBaseConfig(const std::filesystem::path &path)
{
    writeFile(path, R"(viewerUrl = "https://toml-viewer.example/"
bytebinUrl = "https://toml-upload.example/"
bytesocksHost = "toml-socket.example"
backgroundProfiler = false
backgroundProfilerInterval = 24
backgroundProfilerThreadGrouper = "by-name"
backgroundProfilerThreadDumper = "all"
allocationRateMetrics = false
disableResponseBroadcast = false
)");
}

void assertDefaults(const SparkConfig &config)
{
    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.bytebin_url == "https://spark-usercontent.lucko.me/");
    assert(config.bytesocks_host == "spark-usersockets.lucko.me");
    assert(config.background_profiler_enabled);
    assert(config.background_profiler_interval == 10);
    assert(config.background_profiler_thread_grouper == "by-pool");
    assert(config.background_profiler_thread_dumper == "default");
    assert(!config.allocation_rate_metrics_enabled);
    assert(!config.disable_response_broadcast);
}

}  // namespace

void testEnvironmentOverrides()
{
    const auto path = tempDir() / "environment_override.toml";
    cleanup(path);
    writeBaseConfig(path);
    ScopedEnvironment environment(KEnvironmentNames);
    setEnvironment("SPARK_VIEWERURL", "https://env-viewer.example");
    setEnvironment("SPARK_BYTEBINURL", "https://env-upload.example");
    setEnvironment("SPARK_BYTESOCKSHOST", "env-socket.example:9443");
    setEnvironment("SPARK_BACKGROUNDPROFILER", "TrUe");
    setEnvironment("SPARK_BACKGROUNDPROFILERINTERVAL", "37");
    setEnvironment("SPARK_BACKGROUNDPROFILERTHREADGROUPER", "as-one");
    setEnvironment("SPARK_BACKGROUNDPROFILERTHREADDUMPER", "default");
    setEnvironment("SPARK_ALLOCATIONRATEMETRICS", "TRUE");
    setEnvironment("SPARK_DISABLERESPONSEBROADCAST", "TRUE");

    SparkConfig config(path);
    assert(config.load());
    assert(config.viewer_url == "https://env-viewer.example/");
    assert(config.bytebin_url == "https://env-upload.example/");
    assert(config.bytesocks_host == "env-socket.example:9443");
    assert(config.background_profiler_enabled);
    assert(config.background_profiler_interval == 37);
    assert(config.background_profiler_thread_grouper == "as-one");
    assert(config.background_profiler_thread_dumper == "default");
    assert(config.allocation_rate_metrics_enabled);
    assert(config.disable_response_broadcast);
    std::printf("  [PASS] environment overrides and precedence\n");
}

void testEnvironmentMissingAndExactNames()
{
    const auto path = tempDir() / "environment_missing.toml";
    cleanup(path);
    writeBaseConfig(path);
    ScopedEnvironment environment(KEnvironmentNames);
    setEnvironment("SPARK_VIEWER_URL", "https://wrong-name.example");

    SparkConfig config(path);
    assert(config.load());
    assert(config.viewer_url == "https://toml-viewer.example/");
    assert(config.bytebin_url == "https://toml-upload.example/");
    assert(config.bytesocks_host == "toml-socket.example");
    assert(!config.background_profiler_enabled);
    assert(config.background_profiler_interval == 24);
    assert(config.background_profiler_thread_grouper == "by-name");
    assert(config.background_profiler_thread_dumper == "all");
    assert(!config.allocation_rate_metrics_enabled);
    assert(!config.disable_response_broadcast);
    std::printf("  [PASS] environment missing values and exact names\n");
}

void testEnvironmentInvalidIntegerFallback()
{
    const auto path = tempDir() / "environment_invalid_integer.toml";
    cleanup(path);
    writeBaseConfig(path);
    ScopedEnvironment environment(KEnvironmentNames);
    setEnvironment("SPARK_BACKGROUNDPROFILERINTERVAL", "not-an-integer");

    SparkConfig config(path);
    assert(config.load());
    assert(config.background_profiler_interval == 24);
    std::printf("  [PASS] invalid environment integer fallback\n");
}

void testEnvironmentBooleanParsing()
{
    const auto path = tempDir() / "environment_boolean.toml";
    cleanup(path);
    writeBaseConfig(path);
    for (const std::string value : {"true", "TRUE", "TrUe"}) {
        ScopedEnvironment environment(KEnvironmentNames);
        setEnvironment("SPARK_BACKGROUNDPROFILER", value);
        SparkConfig config(path);
        assert(config.load());
        assert(config.background_profiler_enabled);
    }
    for (const std::string value : {"false", "yes", "", "1"}) {
        ScopedEnvironment environment(KEnvironmentNames);
        setEnvironment("SPARK_BACKGROUNDPROFILER", value);
        SparkConfig config(path);
        assert(config.load());
        assert(!config.background_profiler_enabled);
    }
    std::printf("  [PASS] Java-compatible environment booleans\n");
}

void testEnvironmentValidationIsAtomic()
{
    const auto path = tempDir() / "environment_validation.toml";
    const std::vector<std::pair<const char *, std::string>> invalid = {
        {"SPARK_VIEWERURL", "https://secret-invalid.example/?token=secret"},
        {"SPARK_BYTEBINURL", "not-an-endpoint"},
        {"SPARK_BYTESOCKSHOST", "socket.example/path"},
        {"SPARK_BACKGROUNDPROFILERINTERVAL", "0"},
        {"SPARK_BACKGROUNDPROFILERINTERVAL", "1001"},
        {"SPARK_BACKGROUNDPROFILERTHREADGROUPER", "invalid-grouper"},
        {"SPARK_BACKGROUNDPROFILERTHREADDUMPER", "invalid-dumper"}};
    for (const auto &[name, value] : invalid) {
        cleanup(path);
        writeBaseConfig(path);
        ScopedEnvironment environment(KEnvironmentNames);
        setEnvironment(name, value);
        SparkConfig config(path);
        assert(!config.load());
        assertDefaults(config);
        assert(config.lastError().find("secret") == std::string::npos);
    }
    std::printf("  [PASS] environment validation is atomic\n");
}

void testEnvironmentLoadOrCreateDoesNotPersist()
{
    const auto path = tempDir() / "environment_load_or_create.toml";
    cleanup(path);
    {
        ScopedEnvironment environment(KEnvironmentNames);
        setEnvironment("SPARK_VIEWERURL", "https://environment-only.example/");
        setEnvironment("SPARK_BACKGROUNDPROFILERINTERVAL", "42");
        SparkConfig config(path);
        assert(config.loadOrCreate());
        assert(config.viewer_url == "https://environment-only.example/");
        assert(config.background_profiler_interval == 42);
        std::ifstream in(path, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(content.find("environment-only.example") == std::string::npos);
        assert(content.find("https://spark.lucko.me/") != std::string::npos);
    }
    SparkConfig reloaded(path);
    assert(reloaded.load());
    assertDefaults(reloaded);
    std::printf("  [PASS] loadOrCreate does not persist environment overrides\n");
}

}  // namespace spark::config_test
