#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/metadata/server_properties.h"

namespace {

std::filesystem::path writePropertiesFixture()
{
    const auto path = std::filesystem::temp_directory_path() / "spark_server_properties_security.properties";
    std::ofstream out(path, std::ios::binary);
    out << "server-name=private server name\n"
           "level-name=private-world-folder\n"
           "level-seed=super-secret-seed\n"
           "script-debugger-passcode=123456\n"
           "script-debugger-auto-attach-connect-address=debug.internal:19144\n"
           "third-party-token=token-value\n"
           "unknown-key=unknown-value\n"
           "custom-safe-key=custom-value\n"
           "server-port=19132\n"
           "gamemode=survival\n"
           "enable-lan-visibility=false\n"
           "player-position-acceptance-threshold=0.75\n"
           "server-authoritative-block-breaking-range-scalar=1.5\n"
           "script-watchdog-enable-exception-handling=true\n"
           "script-watchdog-memory-warning=150\n"
           "script-watchdog-memory-limit=300\n"
           "diagnostics-capture-auto-start=false\n";
    out.close();
    return path;
}

}  // namespace

int main()
{
    using spark::parseServerProperties;
    using spark::serverPropertiesToJsonString;
    using spark::setAdditionalSafeServerPropertyKeys;

    assert(serverPropertiesToJsonString({}) == "{}");

    {
        const std::map<std::string, std::string> properties = {{"flag", "true"}, {"other", "false"}};
        assert(serverPropertiesToJsonString(properties) == "{\"flag\":true,\"other\":false}");
    }
    {
        const std::map<std::string, std::string> properties = {{"max-players", "20"}, {"port", "19132"}};
        assert(serverPropertiesToJsonString(properties) == "{\"max-players\":20,\"port\":19132}");
    }
    {
        const std::map<std::string, std::string> properties = {
            {"empty-prefix", "00"}, {"leading-zero", "01"}, {"ten", "10"}, {"zero", "0"}};
        assert(serverPropertiesToJsonString(properties) ==
               "{\"empty-prefix\":\"00\",\"leading-zero\":\"01\",\"ten\":10,\"zero\":0}");
    }
    {
        const std::map<std::string, std::string> properties = {{"level", "info"}, {"algo", "snappy"}};
        assert(serverPropertiesToJsonString(properties) == "{\"algo\":\"snappy\",\"level\":\"info\"}");
    }
    {
        const std::map<std::string, std::string> properties = {
            {"max-players", "20"},
            {"client-side-chunk-generation-enabled", "true"},
            {"compression-algorithm", "snappy"},
            {"server-build-radius-ratio", "Disabled"},
        };
        assert(serverPropertiesToJsonString(properties) == "{\"client-side-chunk-generation-enabled\":true,"
                                                           "\"compression-algorithm\":\"snappy\","
                                                           "\"max-players\":20,"
                                                           "\"server-build-radius-ratio\":\"Disabled\"}");
    }
    {
        const std::map<std::string, std::string> properties = {{"key", "a\"b\\c"}};
        assert(serverPropertiesToJsonString(properties) == "{\"key\":\"a\\\"b\\\\c\"}");
    }

    const auto path = writePropertiesFixture();

    // Default policy is a strict known-safe allowlist. Unknown, identity-bearing,
    // credential-like and deliberately conservative keys never leak by default.
    {
        const auto properties = parseServerProperties(path);
        assert(properties.contains("enable-lan-visibility"));
        assert(properties.contains("player-position-acceptance-threshold"));
        assert(properties.contains("server-authoritative-block-breaking-range-scalar"));
        assert(properties.contains("script-watchdog-enable-exception-handling"));
        assert(properties.contains("script-watchdog-memory-warning"));
        assert(properties.contains("script-watchdog-memory-limit"));
        assert(properties.contains("diagnostics-capture-auto-start"));
        assert(!properties.contains("server-name"));
        assert(!properties.contains("level-name"));
        assert(!properties.contains("level-seed"));
        assert(!properties.contains("script-debugger-passcode"));
        assert(!properties.contains("script-debugger-auto-attach-connect-address"));
        assert(!properties.contains("third-party-token"));
        assert(!properties.contains("unknown-key"));
        assert(!properties.contains("custom-safe-key"));
        assert(!properties.contains("server-port"));
        assert(!properties.contains("gamemode"));
    }

    // An administrator can append reviewed non-sensitive keys. Sensitive-name
    // guards remain authoritative even if a key is accidentally configured.
    {
        const std::vector<std::string> additional = {
            "server-port", "gamemode", "custom-safe-key", "script-debugger-passcode", "third-party-token"};
        const auto properties = parseServerProperties(path, additional);
        assert(properties.at("server-port") == "19132");
        assert(properties.at("gamemode") == "survival");
        assert(properties.at("custom-safe-key") == "custom-value");
        assert(!properties.contains("script-debugger-passcode"));
        assert(!properties.contains("third-party-token"));
    }

    // Startup-configured extensions are applied to ordinary parser calls.
    {
        setAdditionalSafeServerPropertyKeys({"server-port"});
        const auto properties = parseServerProperties(path);
        assert(properties.at("server-port") == "19132");
        setAdditionalSafeServerPropertyKeys({});
    }

    std::filesystem::remove(path);
    return 0;
}
