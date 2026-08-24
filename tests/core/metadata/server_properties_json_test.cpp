#include <cassert>
#include <map>
#include <string>

#include "core/metadata/server_properties.h"

int main()
{
    using spark::serverPropertiesToJsonString;

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
    return 0;
}
