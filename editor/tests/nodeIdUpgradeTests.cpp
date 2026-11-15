#include "../../engine/tests/testHarness.h"

#include "build/nodeIdUpgrade.h"

#include "engine/core/util/jsonTokens.h"
#include "engine/runtime/assetStore/modelImporter.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
std::string readText(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeText(const fs::path& path, const std::string& text)
{
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// A file in the editor's own style.
void writeFile(const fs::path& path, const json& contents)
{
  fs::create_directories(path.parent_path());
  sfs::SceneSerializer::writeJsonFile(contents, path);
}

void writeMeta(const fs::path& file, const std::string& id)
{
  writeText(fs::path(file.string() + ".meta"), R"({"formatVersion": 1, "id": ")" + id + R"("})");
}

// A model of two one-triangle meshes. In the file's order its nodes are Body (1), Arm (2), Hand (3), Wheel (4),
// Wheel again (5), an unnamed one (6) and Cab (7).
void writeModel(const fs::path& file)
{
  // Three float positions (0,0,0), (1,0,0), (0,1,0) and the indices 0, 1, 2 (padded), as base64.
  const std::string buffer = "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA=";
  writeText(file, R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0, 6]}],
    "nodes": [
      {"name": "Body", "mesh": 0, "children": [1, 3, 4, 5]},
      {"name": "Arm", "children": [2]},
      {"name": "Hand"},
      {"name": "Wheel"},
      {"name": "Wheel"},
      {},
      {"name": "Cab", "mesh": 1}
    ],
    "meshes": [
      {"name": "A", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]},
      {"name": "B", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}
    ],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
    ],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}
    ],
    "buffers": [{"byteLength": 44, "uri": "data:application/octet-stream;base64,)" +
                       buffer + R"("}]
  })");
}

json named(const std::string& name, json extra = json::object())
{
  extra["NameComponent"] = {{"name", name}};
  return extra;
}

json moved(float z) { return {{"TransformComponent", {{"position", {0, 0, z}}}}}; }

constexpr const char* kModelId = "3123456789abcdef0123456789abcdef";
constexpr const char* kMachineId = "4123456789abcdef0123456789abcdef";
constexpr const char* kOuterId = "5123456789abcdef0123456789abcdef";

// A project whose prefabs and scene were saved with the model's old ids.
fs::path writeProject()
{
  const fs::path project = fs::temp_directory_path() / "sfs-node-id-upgrade-test";
  fs::remove_all(project);
  writeModel(project / "assets" / "machine.gltf");
  writeMeta(project / "assets" / "machine.gltf", kModelId);
  const std::string model = std::string("asset:") + kModelId;
  const std::string machine = std::string("asset:") + kMachineId;

  // The machine: the model as its entity 1, overriding the arm, the second wheel and the unnamed node, removing the
  // first wheel, adding a glove under the hand.
  json machineRoot = named("Machine");
  machineRoot["children"] = json::array(
      {{{"$id", 1},
        {"$prefab", model},
        {"$overrides", {{"2", moved(2)}, {"5", moved(5)}, {"6", moved(6)}, {"", moved(1)}}},
        {"$removed", {"4"}},
        {"$added", {named("Glove", {{"$parent", "3"}})}}},
       named("Seat", {{"$id", 2}})});
  writeFile(project / "prefabs" / "Machine.prefab", {{"formatVersion", 1}, {"root", machineRoot}});
  writeMeta(project / "prefabs" / "Machine.prefab", kMachineId);

  // A shed holding a machine, moving its cab.
  json outerRoot = named("Shed");
  outerRoot["children"] =
      json::array({{{"$id", 3}, {"$prefab", machine}, {"$overrides", {{"1/7", moved(7)}, {"2", moved(8)}}}}});
  writeFile(project / "prefabs" / "Shed.prefab", {{"formatVersion", 1}, {"root", outerRoot}});
  writeMeta(project / "prefabs" / "Shed.prefab", kOuterId);

  // A prefab with no model in it.
  json plainRoot = named("Fence");
  plainRoot["children"] = json::array({named("Rail", {{"$id", 1}})});
  writeFile(project / "prefabs" / "Fence.prefab", {{"formatVersion", 1}, {"root", plainRoot}});
  writeMeta(project / "prefabs" / "Fence.prefab", "6123456789abcdef0123456789abcdef");

  // A scene moving one machine's cab, and a shed overriding only its own root.
  writeFile(project / "scenes" / "farm.scene",
            {{"formatVersion", 2},
             {"systems", json::array()},
             {"entities",
              {{{"$prefab", machine}, {"$overrides", {{"1/7", moved(9)}}}},
               {{"$prefab", std::string("asset:") + kOuterId}, {"$overrides", {{"", moved(3)}}}},
               named("Sun")}}});
  writeFile(project / "scenes" / "empty.scene",
            {{"formatVersion", 2}, {"systems", json::array()}, {"entities", {named("Sun")}}});
  return project;
}

// Whether `after` differs from `before` only in the strings in `changed` (old text -> new) and formatVersion: every
// token in the same place, every byte between tokens the same.
bool onlyTokensChanged(const std::string& before, const std::string& after, const std::set<std::string>& changed)
{
  const auto a = sfs::scanJsonTokens(before);
  const auto b = sfs::scanJsonTokens(after);
  if (!a || !b || a->size() != b->size())
    return false;
  std::size_t endA = 0, endB = 0;
  for (std::size_t i = 0; i < a->size(); ++i)
  {
    const sfs::JsonToken& x = (*a)[i];
    const sfs::JsonToken& y = (*b)[i];
    if (before.substr(endA, x.begin - endA) != after.substr(endB, y.begin - endB) || x.kind != y.kind)
      return false;
    if (x.text != y.text && !changed.contains(x.text + " -> " + y.text) && x.pointer != "/formatVersion")
      return false;
    endA = x.end;
    endB = y.end;
  }
  return before.substr(endA) == after.substr(endB);
}
} // namespace

int main()
{
  const fs::path project = writeProject();
  const std::vector<std::string> names = {"prefabs/Machine.prefab",
                                          "prefabs/Shed.prefab",
                                          "prefabs/Fence.prefab",
                                          "scenes/farm.scene",
                                          "scenes/empty.scene"};
  std::vector<std::string> original;
  for (const std::string& name : names)
    original.push_back(readText(project / name));

  std::vector<std::string> paths;
  const auto nodes = sfs::importModelNodes(project / "assets" / "machine.gltf", {}, &paths);
  CHECK(nodes.has_value() && nodes->size() == 7u);
  if (!nodes || nodes->size() != 7u)
    return testing::report("nodeIdUpgradeTests");
  const auto id = [&](int old) { return std::to_string((*nodes)[old - 1].id); };

  TEST("--check should find every old id, pass, and write nothing")
  {
    const NodeIdUpgradeReport report = upgradeNodeIds(project, false);
    CHECK(report.ok);
    CHECK(report.files == 5u);
    CHECK(report.changed.size() == 3u);
    if (report.changed.size() == 3u)
    {
      CHECK(report.changed[0].path == "prefabs/Machine.prefab");
      CHECK(report.changed[0].edits == 5u);
      CHECK(report.changed[1].path == "prefabs/Shed.prefab");
      CHECK(report.changed[2].path == "scenes/farm.scene");
    }
    CHECK(report.overrideKeys == 5u);
    CHECK(report.removedPaths == 1u);
    CHECK(report.addedParents == 1u);
    for (std::size_t i = 0; i < names.size(); ++i)
      CHECK(readText(project / names[i]) == original[i]);
  }

  TEST("upgrading should change only the old ids and formatVersion")
  {
    const NodeIdUpgradeReport report = upgradeNodeIds(project, true);
    CHECK(report.ok);
    const std::set<std::string> changed = {"2 -> " + id(2),
                                           "5 -> " + id(5),
                                           "6 -> " + id(6),
                                           "4 -> " + id(4),
                                           "3 -> " + id(3),
                                           "1/7 -> 1/" + id(7)};
    for (std::size_t i = 0; i < names.size(); ++i)
    {
      const std::string text = readText(project / names[i]);
      CHECK(onlyTokensChanged(original[i], text, changed));
      CHECK((text != original[i]) == (i == 0 || i == 1 || i == 3));
    }
    const json machine = json::parse(readText(project / names[0]));
    CHECK(machine["formatVersion"] == sfs::SceneSerializer::kPrefabFormatVersion);
    const json& instance = machine["root"]["children"][0];
    CHECK(instance["$overrides"].contains(id(5)));
    CHECK(instance["$overrides"].contains(""));
    CHECK(instance["$removed"] == json::array({id(4)}));
    CHECK(instance["$added"][0]["$parent"] == id(3));
    // The unnamed node and the second wheel are named by their numbered paths.
    CHECK(paths[4] == "Body/Wheel#1");
    CHECK(paths[5] == "Body/");
    const json scene = json::parse(readText(project / names[3]));
    CHECK(scene["formatVersion"] == sfs::SceneSerializer::kSceneFormatVersion);
    CHECK(json::parse(readText(project / names[4]))["formatVersion"] == 2);
  }

  TEST("a second run should change nothing")
  {
    std::vector<std::string> upgraded;
    for (const std::string& name : names)
      upgraded.push_back(readText(project / name));
    const NodeIdUpgradeReport report = upgradeNodeIds(project, true);
    CHECK(report.ok);
    CHECK(report.changed.empty());
    for (std::size_t i = 0; i < names.size(); ++i)
      CHECK(readText(project / names[i]) == upgraded[i]);
  }

  TEST("no staged file should be left beside an upgraded one")
  {
    for (const auto& entry : fs::recursive_directory_iterator(project))
      CHECK(entry.path().extension() != ".upgrading");
  }

  TEST("an old id past the model's nodes should be kept as old:<id>")
  {
    const fs::path past = writeProject();
    json machine = json::parse(readText(past / names[0]));
    machine["root"]["children"][0]["$overrides"]["9"] = moved(9);
    writeFile(past / names[0], machine);
    const NodeIdUpgradeReport report = upgradeNodeIds(past, true);
    CHECK(report.ok);
    CHECK(report.oldIdsKept == 1u);
    const json upgraded = json::parse(readText(past / names[0]));
    CHECK(upgraded["root"]["children"][0]["$overrides"].contains("old:9"));
    CHECK(upgradeNodeIds(past, false).changed.empty());
  }

  TEST("an instance whose model can't be read should fail the check, and "
       "change nothing")
  {
    const fs::path missing = writeProject();
    fs::remove(missing / "assets" / "machine.gltf.meta");
    fs::remove(missing / "assets" / "machine.gltf");
    std::vector<std::string> before;
    for (const std::string& name : names)
      before.push_back(readText(missing / name));
    const NodeIdUpgradeReport report = upgradeNodeIds(missing, true);
    CHECK(!report.ok);
    CHECK(!report.unreadable.empty());
    if (!report.unreadable.empty())
      CHECK(report.unreadable[0] == std::string("prefabs/Machine.prefab: asset:") + kModelId);
    CHECK(upgradeNodeIdsHeadless(missing, true) != 0);
    for (std::size_t i = 0; i < names.size(); ++i)
      CHECK(readText(missing / names[i]) == before[i]);
  }

  TEST("a file the checks fail on should leave every file untouched")
  {
    const fs::path broken = writeProject();
    // A machine without a formatVersion can't be upgraded by its tokens alone.
    json machine = json::parse(readText(broken / names[0]));
    machine.erase("formatVersion");
    writeFile(broken / names[0], machine);
    std::vector<std::string> before;
    for (const std::string& name : names)
      before.push_back(readText(broken / name));
    const NodeIdUpgradeReport report = upgradeNodeIds(broken, true);
    CHECK(!report.ok);
    for (std::size_t i = 0; i < names.size(); ++i)
      CHECK(readText(broken / names[i]) == before[i]);
  }

  TEST("--list-node-ids should read a model, and fail on a missing one")
  {
    CHECK(listNodeIdsHeadless(project / "assets" / "machine.gltf") == 0);
    CHECK(listNodeIdsHeadless(project / "assets" / "missing.gltf") != 0);
    CHECK(upgradeNodeIdsHeadless(project / "missing", true) != 0);
  }

  fs::remove_all(project);
  return testing::report("nodeIdUpgradeTests");
}
