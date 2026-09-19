#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "proto/proto_reader.h"
#include "proto/sampler_data.h"
#include "proto_test_utils.h"

namespace {

struct MethodCase {
    std::string input;
    std::string expected;
};

std::vector<std::string> serializedMethods(std::string_view profile)
{
    std::vector<std::string> methods;
    spark::proto_test::findMessageBytes(profile, 2, [&](std::string_view thread) {
        spark::proto_test::findMessageBytes(thread, 3, [&](std::string_view node) {
            spark::ProtoReader reader(node);
            int field = 0;
            int wire_type = 0;
            while (reader.nextField(field, wire_type)) {
                if (field == 4 && wire_type == 2) {
                    methods.emplace_back(reader.readString());
                }
                else {
                    reader.skip(wire_type);
                }
            }
            return false;
        });
        return false;
    });
    return methods;
}

}  // namespace

int main()
{
    const std::vector<MethodCase> cases = {
        {R"(lambda at C:\Users\Alice\src\bedrock.cpp:12:34')", R"(lambda at bedrock.cpp:12:34')"},
        {R"(lambda at D:/Build/src/forward.cpp:22:3')", R"(lambda at forward.cpp:22:3')"},
        {R"(lambda at C:\Users\O'Connor\src\server.cpp:17:5')", R"(lambda at server.cpp:17:5')"},
        {R"(lambda at \\server\share\src\server.cpp:56:7')", R"(lambda at server.cpp:56:7')"},
        {R"(lambda at /home/alice/src/server.cpp:89:10')", R"(lambda at server.cpp:89:10')"},
        {R"(lambda at /home/O'Connor/src/server.cpp:27:9')", R"(lambda at server.cpp:27:9')"},
        {R"(call lambda at C:\Users\Alice\one.cpp:1:2' + lambda at /home/alice/two.cpp:3:4')",
         R"(call lambda at one.cpp:1:2' + lambda at two.cpp:3:4')"},
        {"ordinary /home/alice/source.cpp", "ordinary /home/alice/source.cpp"},
        {R"(lambda at C:\Users\Alice\bad.cpp:12:x')", R"(lambda at C:\Users\Alice\bad.cpp:12:x')"},
        {R"(prefix lambda at C:\Users\Alice\bad.cpp:12:x' then lambda at /tmp/good.cpp:7:8')",
         R"(prefix lambda at C:\Users\Alice\bad.cpp:12:x' then lambda at good.cpp:7:8')"},
        {R"(lambda at /tmp/zero.cpp:0:2')", R"(lambda at /tmp/zero.cpp:0:2')"},
        {R"(lambda at src/relative.cpp:4:5')", R"(lambda at src/relative.cpp:4:5')"},
    };

    spark::ModuleTable modules;
    spark::CallTree tree;
    std::unordered_map<spark::FrameKey, spark::ResolvedFrame, spark::FrameKeyHash> resolved;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const spark::FrameKey frame{.module = modules.intern("bedrock_server"),
                                    .rva = static_cast<std::uint64_t>(i + 1),
                                    .raw_address = static_cast<std::uint64_t>(i + 1)};
        const std::uint64_t weight = static_cast<std::uint64_t>(i + 1);
        tree.log({frame}, static_cast<std::int32_t>(i), weight);
        resolved.emplace(frame, spark::ResolvedFrame{.class_name = "bedrock_server", .method_name = cases[i].input});
    }

    const std::string profile = spark::buildSamplerData({}, tree, resolved);
    const std::vector<std::string> methods = serializedMethods(profile);
    if (methods.size() != cases.size()) {
        std::fprintf(stderr, "sampler data privacy: expected %zu methods, got %zu\n", cases.size(), methods.size());
        return 1;
    }
    for (const MethodCase &test : cases) {
        bool found = false;
        for (const std::string &method : methods) {
            found = found || method == test.expected;
        }
        if (!found) {
            std::fprintf(stderr, "sampler data privacy: missing serialized method %s\n", test.expected.c_str());
            return 1;
        }
        if (test.input != test.expected && profile.find(test.input) != std::string::npos) {
            std::fprintf(stderr, "sampler data privacy: absolute source path leaked in %s\n", test.input.c_str());
            return 1;
        }
    }
    if (profile.find(R"(C:\Users\O'Connor\src\server.cpp)") != std::string::npos ||
        profile.find(R"(/home/O'Connor/src/server.cpp)") != std::string::npos ||
        profile.find(R"(/tmp/good.cpp)") != std::string::npos) {
        std::fprintf(stderr, "sampler data privacy: apostrophe path leaked\n");
        return 1;
    }

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const spark::FrameKey frame{.module = 0, .rva = static_cast<std::uint64_t>(i + 1)};
        const auto resolved_frame = resolved.find(frame);
        if (resolved_frame == resolved.end() || resolved_frame->second.method_name != cases[i].input) {
            std::fprintf(stderr, "sampler data privacy: resolved frame was mutated\n");
            return 1;
        }
        const auto node = tree.root().children.find(frame);
        if (node == tree.root().children.end() || node->second->times.at(static_cast<std::int32_t>(i)) != i + 1) {
            std::fprintf(stderr, "sampler data privacy: aggregation was mutated\n");
            return 1;
        }
    }
    return 0;
}
