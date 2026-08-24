#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#include "core/config/spark_config.h"
#include "spark_config_test_cases.h"
#include "spark_config_test_support.h"

namespace spark::config_test {

void testSaveCreatesToml()
{
    const auto path = tempDir() / "save_create.toml";
    cleanup(path);
    SparkConfig config(path);
    assert(config.save());
    assert(std::filesystem::exists(path));
    SparkConfig reloaded(path);
    assert(reloaded.load());
    assert(reloaded.viewer_url == "https://spark.lucko.me/");
    std::ifstream in(path);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(content.find("# spark configuration") != std::string::npos);
    assert(content.find("viewerUrl") != std::string::npos);
    std::printf("  [PASS] save creates TOML\n");
}

void testSaveAndReload()
{
    const auto path = tempDir() / "save_reload.toml";
    cleanup(path);
    SparkConfig config(path);
    config.save();
    config.load();
    config.viewer_url = "https://saved.example.com/";
    config.bytebin_url = "https://upload-saved.example.com/";
    config.background_profiler_enabled = false;
    config.background_profiler_interval = 25;
    assert(config.save());
    SparkConfig reloaded(path);
    assert(reloaded.load());
    assert(reloaded.viewer_url == "https://saved.example.com/");
    assert(reloaded.bytebin_url == "https://upload-saved.example.com/");
    assert(!reloaded.background_profiler_enabled);
    assert(reloaded.background_profiler_interval == 25);
    std::printf("  [PASS] save and reload\n");
}

void testEmptyToml()
{
    const auto path = tempDir() / "empty.toml";
    cleanup(path);
    writeFile(path, "");
    SparkConfig config(path);
    assert(config.load());
    assert(config.viewer_url == "https://spark.lucko.me/");
    assert(config.background_profiler_enabled);
    std::printf("  [PASS] empty TOML\n");
}

void testTomlWithComments()
{
    const auto path = tempDir() / "with_comments.toml";
    cleanup(path);
    writeFile(path, R"(# This is a comment
# Another comment
viewerUrl = "https://commented.example.com/"
# Trailing comment
)");
    SparkConfig config(path);
    assert(config.load());
    assert(config.viewer_url == "https://commented.example.com/");
    std::printf("  [PASS] TOML with comments\n");
}

void testBoundedAndTrailingInput()
{
    const auto path = tempDir() / "bounded.toml";
    writeFile(path, "viewerUrl = \"https://example.com/\"\ntrailing data\n");
    SparkConfig trailing(path);
    assert(!trailing.load());
    assert(!trailing.lastError().empty());
    writeFile(path, std::string(1U * 1024U * 1024U + 1U, '#'));
    SparkConfig oversized(path);
    assert(!oversized.load());
    assert(!oversized.lastError().empty());
    std::printf("  [PASS] bounded and trailing input\n");
}

}  // namespace spark::config_test
