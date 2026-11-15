#include "../../engine/tests/runtime/serialization/rigComponent.h"
#include "../../engine/tests/testHarness.h"
#include "partsTestSupport.h"

#include "build/fillParts.h"
#include "scene/entityLabel.h"
#include "scene/parts.h"
#include "scene/references.h"
#include "scene/unusedOverrides.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/registration.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/assetStore/modelImporter.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "engine/runtime/serialization/unresolvedComponents.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using sfs::Entity;

using namespace partsTests;
using sfs::testing3d::LogLines;

namespace
{
// A scene, with prefab files in memory.
struct World
{
  sfs::AssetStore store;
  sfs::Scene scene{0, sfs::SceneServices{.assetStore = store}};
  std::map<std::string, json> prefabs;

  sfs::SceneSerializer serializer()
  {
    sfs::SceneSerializer out(sfs::reflect::types(), nullptr);
    out.setPrefabSource(
        [this](std::string_view name) -> std::optional<json>
        {
          const auto it = prefabs.find(std::string(name));
          return it == prefabs.end() ? std::nullopt
                                     : std::optional<json>(it->second);
        });
    return out;
  }

  Entity make(const std::string& name, const Entity& parent = {})
  {
    Entity entity = scene.createEntity();
    entity.addComponent<sfs::NameComponent>(name);
    if (parent.isValid())
      sfs::hierarchy::setParent(entity, parent);
    return entity;
  }
};

} // namespace

int main()
{
  registerParts();

  TEST("Fill Parts should take the first match for one entity and every match "
       "for a list, in preorder, "
       "past Blender's copy numbers")
  {
    World w;
    Entity machine = w.make("Machine");
    const Entity body = w.make("Body", machine);
    const Entity arm = w.make("Arm.001", body);
    const Entity blade1 = w.make("Blade-1", body);
    const Entity blade2 = w.make("Blade-2", machine);
    const Entity otherArm = w.make("Arm", machine);
    machine.addComponent<PartsComponent>();
    w.scene.flushEntities();

    LogLines log;
    CHECK(parts::fill(machine, partsType()) == 2u);
    const PartsComponent* filled = machine.getComponent<PartsComponent>();
    CHECK(filled->arm == arm);
    CHECK(filled->blades == std::vector<Entity>({blade1, blade2}));
    CHECK(log.count("2 entities below Machine are named as its part") == 1u);
    CHECK(otherArm.isValid());
    // Already filled: nothing changes.
    CHECK(parts::fill(machine, partsType()) == 0u);

    // Only what's empty, as a component just added.
    machine.getComponent<PartsComponent>()->arm = body;
    machine.getComponent<PartsComponent>()->blades.clear();
    CHECK(parts::fill(machine, partsType(), true) == 1u);
    CHECK(machine.getComponent<PartsComponent>()->arm == body);
    CHECK(machine.getComponent<PartsComponent>()->blades.size() == 2u);

    // fillAll: every component below the root too.
    Entity inner = w.make("Inner", machine);
    w.make("Arm", inner);
    inner.addComponent<PartsComponent>();
    w.scene.flushEntities();
    CHECK(parts::fillAll(machine) ==
          2u); // the root's arm, the inner one's arm (no blades below it)
    CHECK(inner.getComponent<PartsComponent>()->arm ==
          childNamed(inner, "Arm"));
  }

  TEST("opening a prefab should refill part fields whose target was renamed or "
       "didn't resolve, and keep one "
       "whose part is gone")
  {
    World w;
    LogLines log;
    // The saved arm names nothing (its node renamed in the model): kept, empty.
    w.prefabs["asset:rig"] = rigPrefab({{"arm", "99"}, {"blades", {"2", "3"}}});
    Entity rig = w.serializer().instantiate("asset:rig", w.scene);
    w.scene.flushEntities();
    CHECK(!rig.getComponent<PartsComponent>()->arm.isValid());
    CHECK(references::keptValue(
              rig, partsType(), partsType().fields[0], references::kNoItem) !=
          nullptr);
    CHECK(*references::keptValue(
              rig, partsType(), partsType().fields[0], references::kNoItem) ==
          "99");

    CHECK(parts::heal(rig, "Rig.prefab") == 1u);
    CHECK(rig.getComponent<PartsComponent>()->arm == childNamed(rig, "Arm"));
    CHECK(log.count(
              "Rig.prefab: Rig PartsComponent.arm no longer names its part") ==
          1u);
    // Once set, the kept value is dropped when saved.
    rig.removeComponent<sfs::PrefabInstanceComponent>();
    CHECK(w.serializer().savePrefab(
              rig, true)["root"]["PartsComponent"]["arm"] == "1");

    // A target named otherwise now is refilled; with no part left, kept.
    childNamed(rig, "Arm").getComponent<sfs::NameComponent>()->name = "Elbow";
    CHECK(parts::heal(rig, "Rig.prefab") == 0u);
    CHECK(rig.getComponent<PartsComponent>()->arm == childNamed(rig, "Elbow"));
    CHECK(log.count("nothing below Rig is named as its part \"Arm\"") == 1u);

    // An empty field stays empty.
    rig.getComponent<PartsComponent>()->arm = Entity();
    CHECK(parts::heal(rig, "Rig.prefab") == 0u);
    CHECK(!rig.getComponent<PartsComponent>()->arm.isValid());

    // A target deleted since it was set is refilled by name, and logged.
    const Entity elbow = childNamed(rig, "Elbow");
    rig.getComponent<PartsComponent>()->arm = elbow;
    w.scene.destroyEntity(elbow);
    const Entity arm = w.make("Arm", rig);
    w.scene.flushEntities();
    const std::size_t logged = log.count(
        "Rig.prefab: Rig PartsComponent.arm no longer names its part");
    CHECK(parts::heal(rig, "Rig.prefab") == 1u);
    CHECK(rig.getComponent<PartsComponent>()->arm == arm);
    CHECK(log.count(
              "Rig.prefab: Rig PartsComponent.arm no longer names its part") ==
          logged + 1);
  }

  TEST("a reference copied to another selected object should name that "
       "object's own part; one outside its "
       "prefab the same entity; one it has no place for nothing")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab({{"arm", "1"}, {"blades", {"2"}}});
    const Entity a = w.serializer().instantiate("asset:rig", w.scene);
    const Entity b = w.serializer().instantiate("asset:rig", w.scene);
    const Entity lamp = w.make("Lamp");
    w.scene.flushEntities();

    const Entity armA = a.getComponent<PartsComponent>()->arm;
    const Entity armB = b.getComponent<PartsComponent>()->arm;
    CHECK(armA != armB);
    const auto& type = partsType();
    const auto& arm = type.fields[0];
    const auto value = [&](const Entity& holder, bool prefabContent = false)
    {
      return references::comparable(
          holder, type, arm, type.component.get(holder), prefabContent);
    };
    // Compared relatively: the same part of each.
    CHECK(value(a) == json("1"));
    CHECK(value(b) == json("1"));
    CHECK(references::comparable(
              a, type, type.fields[1], type.component.get(a), false) ==
          json({"2"}));

    CHECK(references::copied(armA, a, b, false) == std::optional<Entity>(armB));
    CHECK(references::copied(lamp, a, b, false) == std::optional<Entity>(lamp));
    CHECK(references::copied(Entity(), a, b, false) ==
          std::optional<Entity>(Entity()));
    w.scene.destroyBranch(childNamed(b, "Arm"));
    w.scene.flushEntities();
    LogLines log;
    CHECK(!references::copied(armA, a, b, false).has_value());
    CHECK(log.count("has no entity at its place") == 1u);

    // A prefab's content open on its own: named from the top of its branch.
    Entity open = w.make("Open");
    Entity part = w.make("Part", open);
    part.addComponent<sfs::PrefabLinkComponent>(sfs::PrefabLinkComponent{4});
    open.addComponent<PartsComponent>(PartsComponent{part, {}});
    w.scene.flushEntities();
    CHECK(references::contextRoot(part, true) == open);
    CHECK(value(open, true) == json("4"));
    CHECK(references::namePath(part, Entity()) == "Open/Part");
  }

  TEST("a scene's own entities have no context root: a reference to one is "
       "copied as it is")
  {
    World w;
    Entity group1 = w.make("Group 1");
    Entity group2 = w.make("Group 2");
    Entity x = w.make("X", group1);
    Entity y = w.make("Y", group2);
    // Parts of an instance moved out keep their link, so the old fallback to
    // the branch's top would have named the other group.
    x.addComponent<sfs::PrefabLinkComponent>(sfs::PrefabLinkComponent{1});
    y.addComponent<sfs::PrefabLinkComponent>(sfs::PrefabLinkComponent{1});
    x.addComponent<PartsComponent>(PartsComponent{group1, {}});
    y.addComponent<PartsComponent>();
    w.scene.flushEntities();
    const auto& type = partsType();
    CHECK(!references::contextRoot(x, false).isValid());
    CHECK(references::copy(type,
                           type.fields[0],
                           x,
                           type.component.get(x),
                           y,
                           type.component.get(y),
                           false));
    CHECK(y.getComponent<PartsComponent>()->arm == group1);
    // And the two compare as different targets.
    y.getComponent<PartsComponent>()->arm = group2;
    CHECK(references::comparable(
              x, type, type.fields[0], type.component.get(x), false) !=
          references::comparable(
              y, type, type.fields[0], type.component.get(y), false));
  }

  TEST("clearing a Missing reference on several selected objects should clear "
       "each one's kept value, and lists' kept values stay with their items")
  {
    World w;
    w.prefabs["asset:rig"] =
        rigPrefab({{"arm", "99"}, {"blades", {"2", "98", "97"}}});
    const Entity a = w.serializer().instantiate("asset:rig", w.scene);
    const Entity b = w.serializer().instantiate("asset:rig", w.scene);
    w.scene.flushEntities();
    const auto& type = partsType();
    const auto& arm = type.fields[0];
    const auto& blades = type.fields[1];
    const auto value = [&](const Entity& holder, const auto& field)
    {
      return references::comparable(
          holder, type, field, type.component.get(holder), false);
    };
    // Missing isn't None.
    CHECK(value(a, arm) == json({{"missing", "99"}}));
    CHECK(value(a, arm) != json());

    // The first is cleared, and copied to the other, as the Inspector does.
    const json before = value(a, arm);
    references::forgetKept(a, type, arm, references::kNoItem);
    CHECK(value(a, arm) != before); // so the copy happens
    CHECK(references::copy(
        type, arm, a, type.component.get(a), b, type.component.get(b), false));
    // A list item taken out of the first, then copied.
    a.getComponent<PartsComponent>()->blades.erase(
        a.getComponent<PartsComponent>()->blades.begin() + 1);
    references::eraseKeptItem(a, type, blades, 1);
    CHECK(references::copy(type,
                           blades,
                           a,
                           type.component.get(a),
                           b,
                           type.component.get(b),
                           false));
    for (const Entity& each : {a, b})
    {
      const json saved = w.serializer().instanceEntry(each);
      CHECK(saved["$overrides"][""]["PartsComponent"] ==
            json({{"arm", nullptr}, {"blades", {"2", "97"}}}));
    }
  }

  TEST("references held outside an instance should name its new entities once "
       "it's rebuilt")
  {
    World w;
    w.prefabs["asset:rig"] =
        rigPrefab({{"arm", "1"}, {"blades", json::array()}});
    const Entity rig = w.serializer().instantiate("asset:rig", w.scene);
    Entity lamp = w.make("Lamp");
    Entity pole = w.make("Pole");
    w.scene.flushEntities();
    lamp.addComponent<PartsComponent>(
        PartsComponent{childNamed(rig, "Arm"), {rig, pole}});
    w.scene.flushEntities();

    const auto rebuild = [&](const Entity& root)
    {
      const std::vector<references::Inbound> inbound =
          references::inboundTo(root);
      const json entry = w.serializer().instanceEntry(root);
      w.scene.destroyBranch(root);
      w.scene.flushEntities();
      const Entity made = w.serializer().loadEntity(entry, w.scene);
      w.scene.flushEntities();
      references::restore(inbound, made, w.scene);
      return made;
    };

    // By its path in the instance (no scene ids given yet).
    CHECK(references::inboundTo(rig).size() == 2u);
    const Entity again = rebuild(rig);
    CHECK(lamp.getComponent<PartsComponent>()->arm == childNamed(again, "Arm"));
    CHECK(lamp.getComponent<PartsComponent>()->blades[0] == again);
    CHECK(lamp.getComponent<PartsComponent>()->blades[1] == pole);

    // By the scene id a save gave it.
    w.serializer().save(w.scene);
    const std::vector<references::Inbound> inbound =
        references::inboundTo(again);
    CHECK(inbound.size() == 2u && inbound[0].sceneId.has_value());
    const Entity third = rebuild(again);
    CHECK(lamp.getComponent<PartsComponent>()->arm == childNamed(third, "Arm"));
    CHECK(lamp.getComponent<PartsComponent>()->blades[0] == third);
  }

  TEST("a kept reference should be forgotten when cleared, and move up with "
       "its list's items")
  {
    World w;
    w.prefabs["asset:rig"] =
        rigPrefab({{"arm", "98"}, {"blades", {"2", "97", "96"}}});
    const Entity rig = w.serializer().instantiate("asset:rig", w.scene);
    w.scene.flushEntities();
    const auto& arm = partsType().fields[0];
    const auto& blades = partsType().fields[1];
    CHECK(*references::keptValue(rig, partsType(), blades, 1) == "97");
    references::eraseKeptItem(rig, partsType(), blades, 1);
    rig.getComponent<PartsComponent>()->blades.erase(
        rig.getComponent<PartsComponent>()->blades.begin() + 1);
    CHECK(*references::keptValue(rig, partsType(), blades, 1) == "96");
    references::forgetKept(rig, partsType(), arm, references::kNoItem);
    CHECK(references::keptValue(rig, partsType(), arm, references::kNoItem) ==
          nullptr);

    const json saved = w.serializer().instanceEntry(rig);
    CHECK(saved["$overrides"][""]["PartsComponent"] ==
          json({{"arm", nullptr}, {"blades", {"2", "96"}}}));
  }

  TEST("unused overrides of a renamed model node should be reattached with the "
       "nodes below it, or removed")
  {
    World w;
    // The model's nodes as its file has them now: "Arm/2" (a "/" in its name,
    // escaped in its path) was OldArm.
    const std::vector<std::string> paths = {
        "Body", "Body/Arm\\/2", "Body/Arm\\/2/Hand", "Body/Leg"};
    const std::vector<std::string> names = {"Body", "Arm/2", "Hand", "Leg"};
    std::vector<sfs::ModelNode> nodes(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i)
    {
      nodes[i].name = names[i];
      nodes[i].parent = i == 0 ? -1 : (i == 2 ? 1 : 0);
      nodes[i].id = sfs::stableNodeId(paths[i]);
    }
    CHECK(paths[1] == "Body/" + sfs::nodeNameSegment("Arm/2"));
    const auto id = [&](const std::string& path)
    { return std::to_string(sfs::stableNodeId(path)); };
    json body = named("Body", {{"$id", nodes[0].id}});
    json arm = named("Arm/2", {{"$id", nodes[1].id}});
    arm["children"] = json::array({named("Hand", {{"$id", nodes[2].id}})});
    body["children"] = json::array({arm, named("Leg", {{"$id", nodes[3].id}})});
    json model = named("Model");
    model["children"] = json::array({body});
    w.prefabs["asset:model"] = {
        {"formatVersion", 2}, {"model", true}, {"root", model}};

    LogLines log;
    const json entry = {{"$prefab", "asset:model"},
                        {"$overrides",
                         {{id("Body/OldArm"), moved(2)},
                          {id("Body/OldArm/Hand"), moved(3)},
                          {id("Gone"), moved(4)}}},
                        {"$removed", {id("Body/Missing")}}};
    sfs::SceneSerializer serializer = w.serializer();
    Entity root = serializer.loadEntity(entry, w.scene);
    w.scene.flushEntities();
    std::vector<unusedOverrides::Unused> unused = unusedOverrides::of(root);
    CHECK(unused.size() == 4u);

    const Entity armNow = childNamed(childNamed(root, "Body"), "Arm/2");
    const std::map<std::string, std::string> moves =
        unusedOverrides::renamedNodeMoves(root, armNow, "OldArm", nodes, paths);
    CHECK(moves == std::map<std::string, std::string>(
                       {{id("Body/OldArm"), id(paths[1])},
                        {id("Body/OldArm/Hand"), id(paths[2])}}));

    json edited = serializer.instanceEntry(root);
    unusedOverrides::move(edited, moves);
    // One moved onto an override the entry already has merges into it.
    unusedOverrides::move(edited, {{id("Gone"), id("Body/Leg")}});
    unusedOverrides::remove(edited, unused[3]);
    CHECK(!edited.contains("$removed"));
    w.scene.destroyBranch(root);
    w.scene.flushEntities();
    root = serializer.loadEntity(edited, w.scene);
    w.scene.flushEntities();
    CHECK(unusedOverrides::of(root).empty());
    const Entity bodyNow = childNamed(root, "Body");
    CHECK(childNamed(bodyNow, "Arm/2")
              .getComponent<sfs::TransformComponent>()
              ->position.z == 2.0f);
    CHECK(childNamed(childNamed(bodyNow, "Arm/2"), "Hand")
              .getComponent<sfs::TransformComponent>()
              ->position.z == 3.0f);
    CHECK(childNamed(bodyNow, "Leg")
              .getComponent<sfs::TransformComponent>()
              ->position.z == 4.0f);
  }

  TEST("--fill-parts should fill a project's prefabs as Fill Parts does: a dry "
       "run writing nothing")
  {
    const fs::path project = fs::temp_directory_path() / "sfs-fill-parts-test";
    fs::remove_all(project);
    const fs::path rig = project / "prefabs" / "Rig.prefab";
    // Its blades not in the file yet, and a mesh component with only some of
    // its fields: neither may change but for the part values.
    json rigFile = rigPrefab({{"arm", nullptr}});
    const json body = named(
        "Body",
        {{"$id", 4}, {"MeshRendererComponent", {{"mesh", "builtin:cube"}}}});
    rigFile["root"]["children"].push_back(body);
    writeFile(rig, rigFile);
    writeMeta(rig, "7123456789abcdef0123456789abcdef");
    const fs::path fence = project / "prefabs" / "Fence.prefab";
    writeFile(fence, {{"formatVersion", 2}, {"root", named("Fence")}});
    writeMeta(fence, "8123456789abcdef0123456789abcdef");
    const std::string rigBefore = readText(rig);
    const std::string fenceBefore = readText(fence);

    const FillPartsReport dry = fillProjectParts(project, "all", false);
    CHECK(dry.ok);
    CHECK(dry.prefabs == 2u);
    CHECK(dry.changed.size() == 1u &&
          dry.changed[0].path == "prefabs/Rig.prefab" &&
          dry.changed[0].fields == 2u);
    CHECK(readText(rig) == rigBefore);

    const FillPartsReport one =
        fillProjectParts(project, "prefabs/Rig.prefab", true);
    CHECK(one.ok && one.prefabs == 1u && one.changed.size() == 1u);
    CHECK(readText(fence) == fenceBefore);
    // Only the part values' text changed: every other byte is as it was.
    std::string expected = rigBefore;
    const std::string arm = "\"arm\": null";
    const std::size_t at = expected.find(arm);
    CHECK(at != std::string::npos);
    if (at != std::string::npos)
      expected.replace(
          at, arm.size(), "\"arm\": \"1\",\n      \"blades\": [\"2\", \"3\"]");
    CHECK(readText(rig) == expected);
    const std::optional<json> written = sfs::SceneSerializer::readJsonFile(rig);
    CHECK(written && (*written)["root"]["PartsComponent"] ==
                         json({{"arm", "1"}, {"blades", {"2", "3"}}}));
    CHECK(written && (*written)["root"]["children"][3] == body);

    // What it wrote loads as authored references.
    World w;
    w.prefabs["asset:rig"] = *written;
    const Entity loaded = w.serializer().instantiate("asset:rig", w.scene);
    w.scene.flushEntities();
    CHECK(loaded.getComponent<PartsComponent>()->arm ==
          childNamed(loaded, "Arm"));
    CHECK(loaded.getComponent<PartsComponent>()->blades ==
          std::vector<Entity>(
              {childNamed(loaded, "Blade-1"), childNamed(loaded, "Blade-2")}));

    // A second run changes nothing; an unknown prefab is reported.
    CHECK(fillProjectParts(project, "all", true).changed.empty());
    CHECK(!fillProjectParts(project, "prefabs/Nothing.prefab", true).ok);
    fs::remove_all(project);
  }

  return testing::report("referencesTests");
}
