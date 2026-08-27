#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/metadata/behavior_packs.h"

namespace {

const std::string KServerUuid = "11111111-1111-1111-1111-111111111111";
const std::string KWorldUuid = "22222222-2222-2222-2222-222222222222";
const std::string KResourceUuid = "33333333-3333-3333-3333-333333333333";
const std::string KInactiveUuid = "44444444-4444-4444-4444-444444444444";

std::filesystem::path makeTempRoot()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_behavior_pack_metadata_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void writeText(const std::filesystem::path &path, const std::string &text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    stream << text;
    assert(stream.good());
}

std::string manifest(const std::string &uuid, const std::string &version, const std::string &name,
                     const std::string &description, const std::string &module_type)
{
    return "{\n"
           "  \"format_version\": 2,\n"
           "  \"header\": {\n"
           "    \"name\": \"" +
           name +
           "\",\n"
           "    \"description\": \"" +
           description +
           "\",\n"
           "    \"uuid\": \"" +
           uuid +
           "\",\n"
           "    \"version\": " +
           version +
           "\n"
           "  },\n"
           "  \"modules\": [\n"
           "    {\"type\": \"" +
           module_type +
           "\", \"uuid\": \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\", \"version\": [1,0,0]}\n"
           "  ]\n"
           "}\n";
}

const spark::DataPackInfo *findPack(const std::vector<spark::DataPackInfo> &packs, const std::string &name)
{
    const auto it = std::ranges::find_if(packs, [&](const spark::DataPackInfo &pack) { return pack.name == name; });
    return it == packs.end() ? nullptr : &*it;
}

void testSelectedPacksAcrossWorldAndServerRoots()
{
    const auto root = makeTempRoot();
    writeText(root / "server.properties", "level-name=Pack World\n");
    const auto world = root / "worlds" / "Pack World";
    writeText(world / "world_behavior_packs.json",
              "[\n"
              "  // active behavior packs only\n"
              "  {\"pack_id\": \"11111111-1111-1111-1111-111111111111\", \"version\": [1,0,0]},\n"
              "  {\"pack_id\": \"22222222-2222-2222-2222-222222222222\", \"version\": [2,0,0]},\n"
              "  {\"pack_id\": \"33333333-3333-3333-3333-333333333333\", \"version\": [1,0,0]}\n"
              "]\n");

    writeText(root / "behavior_packs" / "server-pack" / "manifest.json",
              manifest(KServerUuid, "[1,0,0]", "Server Pack", "server description", "data"));
    writeText(world / "behavior_packs" / "world-pack" / "manifest.json",
              manifest(KWorldUuid, "[2,0,0]", "World Pack", "world description", "script"));
    writeText(root / "behavior_packs" / "inactive-pack" / "manifest.json",
              manifest(KInactiveUuid, "[1,0,0]", "Inactive Pack", "inactive", "data"));

    // Resource packs have their own stack and directory and must never be mapped
    // onto upstream WorldStatistics.DataPack, even if the UUID appears in a
    // malformed world_behavior_packs.json.
    writeText(world / "resource_packs" / "resource-pack" / "manifest.json",
              manifest(KResourceUuid, "[1,0,0]", "Resource Pack", "resource", "resources"));
    writeText(world / "world_resource_packs.json",
              "[{\"pack_id\": \"33333333-3333-3333-3333-333333333333\", \"version\": [1,0,0]}]\n");

    const auto packs = spark::discoverActiveBehaviorPacks(root, "wrong-hint");
    assert(packs.size() == 2);
    const auto *server_pack = findPack(packs, "Server Pack");
    const auto *world_pack = findPack(packs, "World Pack");
    assert(server_pack != nullptr);
    assert(server_pack->description == "server description");
    assert(server_pack->source == "server");
    assert(!server_pack->builtin);
    assert(world_pack != nullptr);
    assert(world_pack->description == "world description");
    assert(world_pack->source == "world");
    assert(!world_pack->builtin);
    assert(findPack(packs, "Inactive Pack") == nullptr);
    assert(findPack(packs, "Resource Pack") == nullptr);

    std::filesystem::remove_all(root);
}

void testVersionSelectionAndCorruptManifestTolerance()
{
    const auto root = makeTempRoot();
    const auto world = root / "worlds" / "VersionWorld";
    writeText(world / "world_behavior_packs.json",
              "["
              "{\"pack_id\": \"11111111-1111-1111-1111-111111111111\", \"version\": \"2.0.0\"},"
              "{\"pack_id\": \"55555555-5555-5555-5555-555555555555\", \"version\": [1,0,0]}"
              "]");
    writeText(root / "behavior_packs" / "v1" / "manifest.json",
              manifest(KServerUuid, "[1,0,0]", "Old Version", "old", "data"));
    writeText(root / "behavior_packs" / "v2" / "manifest.json",
              manifest(KServerUuid, R"("2.0.0")", "Selected Version", "selected", "data"));
    writeText(root / "behavior_packs" / "broken" / "manifest.json", "{ this is not valid json");

    const auto packs = spark::discoverActiveBehaviorPacks(root, "VersionWorld");
    assert(packs.size() == 1);
    assert(packs.front().name == "Selected Version");
    assert(packs.front().source == "server");

    std::filesystem::remove_all(root);
}

void testMissingManifestAndResourceOnlyModuleAreSkipped()
{
    const auto root = makeTempRoot();
    const auto world = root / "worlds" / "SafeWorld";
    writeText(world / "world_behavior_packs.json",
              "["
              "{\"pack_id\": \"55555555-5555-5555-5555-555555555555\", \"version\": [1,0,0]},"
              "{\"pack_id\": \"33333333-3333-3333-3333-333333333333\", \"version\": [1,0,0]}"
              "]");
    std::filesystem::create_directories(root / "behavior_packs" / "missing-manifest");
    writeText(root / "behavior_packs" / "misplaced-resource" / "manifest.json",
              manifest(KResourceUuid, "[1,0,0]", "Misplaced Resource", "not behavior", "resources"));

    const auto packs = spark::discoverActiveBehaviorPacks(root, "SafeWorld");
    assert(packs.empty());

    std::filesystem::remove_all(root);
}

void testCorruptReferenceFileAndHintFallback()
{
    const auto root = makeTempRoot();
    const auto corrupt_world = root / "worlds" / "CorruptWorld";
    writeText(corrupt_world / "world_behavior_packs.json", "not-json");
    assert(spark::discoverActiveBehaviorPacks(root, "CorruptWorld").empty());

    const auto hint_world = root / "worlds" / "HintWorld";
    writeText(hint_world / "world_behavior_packs.json",
              R"([{"pack_id": "22222222-2222-2222-2222-222222222222", "version": [2,0,0]}])");
    writeText(hint_world / "behavior_packs" / "hint-pack" / "manifest.json",
              manifest(KWorldUuid, "[2,0,0]", "Hint Pack", "hint", "data"));
    const auto packs = spark::discoverActiveBehaviorPacks(root, "HintWorld");
    assert(packs.size() == 1);
    assert(packs.front().name == "Hint Pack");
    assert(packs.front().source == "world");

    std::filesystem::remove_all(root);
}

}  // namespace

int main()
{
    testSelectedPacksAcrossWorldAndServerRoots();
    testVersionSelectionAndCorruptManifestTolerance();
    testMissingManifestAndResourceOnlyModuleAreSkipped();
    testCorruptReferenceFileAndHintFallback();
    return 0;
}
