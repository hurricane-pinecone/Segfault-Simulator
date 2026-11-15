#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"
#include "rigComponent.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/pointLightComponent.h>
#include <engine/core/components/restPoseComponent.h>
#include <engine/core/components/sceneIdComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/reflection/reflection.h>
#include <engine/core/rendering/meshPrimitives.h>
#include <engine/core/util/debugFeatures.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/renderSystem.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/serialization/prefabComponents.h>
#include <engine/runtime/serialization/sceneSerializer.h>
#include <engine/runtime/serialization/unresolvedComponents.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace sfs;
using sfs::testing3d::LogLines;
using sfs::testing3d::MockRenderer;
using sfs::testing3d::PartRigComponent;
using sfs::testing3d::registerRig;
using sfs::testing3d::RigComponent;
using json = nlohmann::json;

namespace
{
// A scene, plus prefab files in memory.
struct World
{
  AssetStore store;
  MockRenderer renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
  RenderSystem& render = scene.addSystem<RenderSystem>(renderer);
  MeshHandle cube = render.meshes().add(meshes::box({1, 1, 1}), "builtin:cube");
  std::map<std::string, json> prefabs;

  SceneSerializer serializer()
  {
    SceneSerializer out(reflect::types(), &render.meshes());
    out.setPrefabSource(
        [this](std::string_view name) -> std::optional<json>
        {
          const auto it = prefabs.find(std::string(name));
          return it == prefabs.end() ? std::nullopt
                                     : std::optional<json>(it->second);
        });
    return out;
  }

  Entity find(const std::string& name)
  {
    scene.update(0.0); // new entities are listed once the scene updates
    for (Entity entity : scene.entities())
      if (const NameComponent* named = entity.getComponent<NameComponent>();
          named && named->name == name)
        return entity;
    return {};
  }

  glm::vec4 tint(const std::string& name)
  {
    return find(name).getComponent<MeshRendererComponent>()->tint;
  }

  // Save, clear and load the scene again.
  void reload()
  {
    const json saved = serializer().save(scene);
    for (Entity entity : scene.entities())
      scene.destroyEntity(entity);
    scene.update(0.0);
    serializer().load(saved, scene);
  }
};

json named(const std::string& name, json extra = json::object())
{
  extra["NameComponent"] = {{"name", name}};
  return extra;
}

// Stable ids of stableModel()'s nodes (as a model's prefab gives them).
constexpr const char* kArm = "2554517965";
constexpr const char* kHand = "2860501858";
constexpr const char* kLeg = "3000000007";

// A model's prefab, as the asset store makes it: an arm with a hand, and a
// leg, each $id a stable id.
json stableModel()
{
  json arm = named(
      "Arm",
      {{"$id", std::stoul(kArm)}, {"TransformComponent", json::object()}});
  arm["children"] = json::array({named(
      "Hand",
      {{"$id", std::stoul(kHand)}, {"TransformComponent", json::object()}})});
  json model = named("Model", {{"TransformComponent", json::object()}});
  model["children"] =
      json::array({arm,
                   named("Leg",
                         {{"$id", std::stoul(kLeg)},
                          {"TransformComponent", json::object()}})});
  return {{"formatVersion", 2}, {"model", true}, {"root", model}};
}

// A cart prefab with the model as its entity 7, overriding, removing and
// adding (a glove under `gloveParent`, if given) by these paths into the
// model.
json cartWithModel(const json& overrides,
                   const std::vector<std::string>& removed,
                   const std::string& gloveParent)
{
  json instance = {{"$id", 7}, {"$prefab", "asset:model"}};
  if (!overrides.empty())
    instance["$overrides"] = overrides;
  if (!removed.empty())
    instance["$removed"] = removed;
  if (!gloveParent.empty())
    instance["$added"] =
        json::array({named("Glove", {{"$parent", gloveParent}})});
  json cart = named("Cart", {{"TransformComponent", json::object()}});
  cart["children"] = json::array({instance});
  return {{"formatVersion", 1}, {"root", cart}};
}

// A fence: a post with two rails, rail 2 red.
json fencePrefab()
{
  json post =
      named("Post", {{"TransformComponent", {{"position", {0, 0, 0}}}}});
  post["children"] = json::array(
      {named("Rail A",
             {{"$id", 1},
              {"MeshRendererComponent",
               {{"mesh", "builtin:cube"}, {"tint", {1, 1, 1, 1}}}}}),
       named("Rail B",
             {{"$id", 2},
              {"MeshRendererComponent",
               {{"mesh", "builtin:cube"}, {"tint", {1, 0, 0, 1}}}}})});
  return {{"formatVersion", 1}, {"root", post}};
}
// A rig: its arm and blades (the middle one none) are its children 1-3.
json rigPrefab()
{
  json rig = named(
      "Rig",
      {{"TransformComponent", json::object()},
       {"RigComponent", {{"arm", "1"}, {"blades", {"2", nullptr, "3"}}}}});
  rig["children"] = json::array({named("Arm", {{"$id", 1}}),
                                 named("Blade A", {{"$id", 2}}),
                                 named("Blade B", {{"$id", 3}})});
  return {{"formatVersion", 2}, {"root", rig}};
}

// A yard: a rig at $id 5 (the yard's arm is that rig's arm), a post at 3, and
// a rig at 7 whose arm is overridden to `rig7Arm` (relative to it).
json yardPrefab(const std::string& rig7Arm)
{
  json yard =
      named("Yard",
            {{"TransformComponent", json::object()},
             {"RigComponent", {{"arm", "5/1"}, {"blades", json::array()}}}});
  yard["children"] = json::array(
      {{{"$id", 5}, {"$prefab", "asset:rig"}},
       named("Post", {{"$id", 3}}),
       {{"$id", 7},
        {"$prefab", "asset:rig"},
        {"$overrides", {{"", {{"RigComponent", {{"arm", rig7Arm}}}}}}}}});
  return {{"formatVersion", 2}, {"root", yard}};
}

// The child of `parent` named `name`.
Entity childNamed(const Entity& parent, const std::string& name)
{
  for (const Entity& child : hierarchy::childrenOf(parent))
    if (const NameComponent* named = child.getComponent<NameComponent>();
        named && named->name == name)
      return child;
  return {};
}

// A saved prefab's child entry with this $id.
const json* childEntry(const json& entry, uint32_t id)
{
  const auto children = entry.find("children");
  if (children != entry.end())
    for (const json& child : *children)
      if (child.value("$id", 0u) == id)
        return &child;
  return nullptr;
}
} // namespace

int main()
{
  registerRig();

  TEST("references in a prefab should resolve to its own entities and save "
       "back as paths, a list keeping its empty items")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    Entity first = w.serializer().instantiate("asset:rig", w.scene);
    const Entity second = w.serializer().instantiate("asset:rig", w.scene);
    for (const Entity& root : {first, second})
    {
      const RigComponent* rig = root.getComponent<RigComponent>();
      CHECK(rig->arm == childNamed(root, "Arm"));
      CHECK(rig->arm.isValid());
      CHECK(rig->blades.size() == 3u);
      CHECK(rig->blades[0] == childNamed(root, "Blade A"));
      CHECK(!rig->blades[1].isValid());
      CHECK(rig->blades[2] == childNamed(root, "Blade B"));
    }
    // Each instance names its own parts.
    CHECK(first.getComponent<RigComponent>()->arm !=
          second.getComponent<RigComponent>()->arm);

    first.removeComponent<PrefabInstanceComponent>(); // as the editor opens it
    const json saved = w.serializer().savePrefab(first, true);
    CHECK(saved["root"]["RigComponent"] ==
          json({{"arm", "1"}, {"blades", {"2", nullptr, "3"}}}));
  }

  TEST("references in nested prefabs should be rebased: into a nested "
       "instance, a nested one's own, and up out of one with '..'")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    w.prefabs["asset:yard"] = yardPrefab("../3");
    Entity yard = w.serializer().instantiate("asset:yard", w.scene);
    w.scene.flushEntities();
    const Entity rig5 = SceneSerializer::entityAtPath(yard, "5");
    const Entity rig7 = SceneSerializer::entityAtPath(yard, "7");
    CHECK(rig5.isValid() && rig7.isValid() && rig5 != rig7);
    CHECK(yard.getComponent<RigComponent>()->arm == childNamed(rig5, "Arm"));
    CHECK(rig5.getComponent<RigComponent>()->arm == childNamed(rig5, "Arm"));
    CHECK(rig7.getComponent<RigComponent>()->arm == childNamed(yard, "Post"));
    CHECK(rig7.getComponent<RigComponent>()->blades[2] ==
          childNamed(rig7, "Blade B"));
    CHECK(SceneSerializer::entityAtPath(yard, "7/3") ==
          childNamed(rig7, "Blade B"));
    CHECK(!SceneSerializer::entityAtPath(yard, "7/9").isValid());
    CHECK(!SceneSerializer::entityAtPath(yard, "x").isValid());

    // An unchanged reference writes no override; the outer one is kept.
    yard.removeComponent<PrefabInstanceComponent>();
    const json saved = w.serializer().savePrefab(yard, true);
    CHECK(saved["root"]["RigComponent"]["arm"] == "5/1");
    CHECK(childEntry(saved["root"], 5) &&
          *childEntry(saved["root"], 5) ==
              json({{"$id", 5}, {"$prefab", "asset:rig"}}));
    CHECK(childEntry(saved["root"], 7) &&
          (*childEntry(saved["root"], 7))["$overrides"] ==
              json({{"", {{"RigComponent", {{"arm", "../3"}}}}}}));

    // A scene's instance of it matches the prefab: no overrides.
    World scene;
    scene.prefabs = w.prefabs;
    scene.serializer().instantiate("asset:yard", scene.scene);
    CHECK(scene.serializer().save(scene.scene)["entities"][0] ==
          json({{"$prefab", "asset:yard"}}));
  }

  TEST("a changed reference in an instance should save as an override and "
       "load back; references across a scene and its instances should be "
       "scene-absolute")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    Entity rig = w.serializer().instantiate("asset:rig", w.scene);
    rig.getComponent<RigComponent>()->arm = childNamed(rig, "Blade B");
    Entity lamp = w.scene.createEntity();
    lamp.addComponent<NameComponent>("Lamp");
    lamp.addComponent<RigComponent>(
        RigComponent{childNamed(rig, "Blade A"), {rig}});
    childNamed(rig, "Arm").addComponent<RigComponent>(RigComponent{lamp, {}});
    w.scene.flushEntities();

    const json saved = w.serializer().save(w.scene);
    const json& instance = saved["entities"][0];
    const json& plain = saved["entities"][1];
    CHECK(instance["$overrides"][""] ==
          json({{"RigComponent", {{"arm", "3"}}}}));
    // Each entry a reference names gets a scene id, the first one 1.
    const std::string rigId = instance.value("$sceneId", json()).dump();
    const std::string lampId = plain.value("$sceneId", json()).dump();
    CHECK(rigId != "null" && lampId != "null" && rigId != lampId);
    CHECK(instance["$overrides"]["1"]["RigComponent"] ==
          json({{"arm", "/" + lampId}, {"blades", json::array()}}));
    CHECK(plain["RigComponent"] ==
          json({{"arm", "/" + rigId + "/2"}, {"blades", {"/" + rigId}}}));

    w.reload();
    const Entity rig2 = w.find("Rig");
    const Entity lamp2 = w.find("Lamp");
    CHECK(rig2.getComponent<RigComponent>()->arm ==
          childNamed(rig2, "Blade B"));
    CHECK(lamp2.getComponent<RigComponent>()->arm ==
          childNamed(rig2, "Blade A"));
    CHECK(lamp2.getComponent<RigComponent>()->blades[0] == rig2);
    CHECK(childNamed(rig2, "Arm").getComponent<RigComponent>()->arm == lamp2);
    // The ids are kept, so saving again changes nothing.
    CHECK(w.serializer().save(w.scene) == saved);
  }

  TEST("a reference to outside a prefab should save as none")
  {
    World w;
    Entity cart = w.scene.createEntity();
    cart.addComponent<NameComponent>("Cart");
    Entity wheel = w.scene.createEntity();
    hierarchy::setParent(wheel, cart);
    const Entity elsewhere = w.scene.createEntity();
    cart.addComponent<RigComponent>(RigComponent{elsewhere, {wheel}});
    const json saved = w.serializer().savePrefab(cart);
    CHECK(saved["root"]["RigComponent"] ==
          json({{"arm", nullptr}, {"blades", {"1"}}}));
  }

  TEST("a reference whose target is missing should load empty, and be saved "
       "back as it was while it stays so")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    w.prefabs["asset:yard"] = yardPrefab("../99");
    json broken = rigPrefab();
    broken["root"]["RigComponent"] = {{"arm", "99"}, {"blades", {"2", "98"}}};
    w.prefabs["asset:broken"] = broken;

    Entity root = w.serializer().instantiate("asset:broken", w.scene);
    const RigComponent* rig = root.getComponent<RigComponent>();
    CHECK(!rig->arm.isValid());
    CHECK(rig->blades.size() == 2u);
    CHECK(rig->blades[0] == childNamed(root, "Blade A"));
    CHECK(!rig->blades[1].isValid());
    CHECK(root.hasComponent<UnresolvedComponents>());
    // Unchanged in a scene, so no override.
    CHECK(w.serializer().save(w.scene)["entities"][0] ==
          json({{"$prefab", "asset:broken"}}));
    root.removeComponent<PrefabInstanceComponent>();
    CHECK(w.serializer().savePrefab(root, true)["root"]["RigComponent"] ==
          broken["root"]["RigComponent"]);

    // One relative to a nested instance, kept as it was too.
    World yards;
    yards.prefabs = w.prefabs;
    Entity yard = yards.serializer().instantiate("asset:yard", yards.scene);
    const Entity rig7 = SceneSerializer::entityAtPath(yard, "7");
    CHECK(!rig7.getComponent<RigComponent>()->arm.isValid());
    CHECK(yards.serializer().save(yards.scene)["entities"][0] ==
          json({{"$prefab", "asset:yard"}}));
    yard.removeComponent<PrefabInstanceComponent>();
    const json saved = yards.serializer().savePrefab(yard, true);
    CHECK(childEntry(saved["root"], 7) &&
          (*childEntry(saved["root"], 7))["$overrides"] ==
              json({{"", {{"RigComponent", {{"arm", "../99"}}}}}}));

    // A scene's own override naming nothing is kept, and setting the field
    // replaces it.
    World scene;
    scene.prefabs = w.prefabs;
    const json file = {
        {"formatVersion", 3},
        {"entities",
         {{{"$prefab", "asset:rig"},
           {"$overrides",
            {{"", {{"RigComponent", {{"arm", "42"}}}}},
             {"1",
              {{"RigComponent",
                {{"arm", "/9"}, {"blades", json::array()}}}}}}}}}}};
    scene.serializer().load(file, scene.scene);
    CHECK(scene.serializer().save(scene.scene)["entities"] == file["entities"]);
    Entity loaded = scene.find("Rig");
    loaded.getComponent<RigComponent>()->arm = childNamed(loaded, "Blade A");
    CHECK(
        scene.serializer().save(scene.scene)["entities"][0]["$overrides"][""] ==
        json({{"RigComponent", {{"arm", "2"}}}}));
  }

  TEST("a copied branch should name what's added to its instances and paths "
       "into them, and an instance's entities its plain ones")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    Entity top = w.scene.createEntity();
    top.addComponent<NameComponent>("Top");
    const Entity rig = w.serializer().instantiate("asset:rig", w.scene, top);
    Entity extra = w.scene.createEntity();
    extra.addComponent<NameComponent>("Extra");
    hierarchy::setParent(extra, childNamed(rig, "Arm"));
    Entity sign = w.scene.createEntity();
    sign.addComponent<NameComponent>("Sign");
    hierarchy::setParent(sign, top);
    sign.addComponent<RigComponent>(
        RigComponent{childNamed(rig, "Blade A"), {extra, rig}});
    childNamed(rig, "Arm").addComponent<RigComponent>(RigComponent{sign, {}});
    w.scene.flushEntities();

    // Top #0, the rig #1, what was added to it #2, the sign #3.
    const json entry = w.serializer().saveEntity(top);
    CHECK(entry["children"][1]["RigComponent"] ==
          json({{"arm", "#1/2"}, {"blades", {"#2", "#1"}}}));
    CHECK(entry["children"][0]["$overrides"]["1"]["RigComponent"]["arm"] ==
          "#3");

    const Entity copy = w.serializer().loadEntity(entry, w.scene);
    w.scene.flushEntities();
    std::vector<Entity> children;
    for (const Entity& child : hierarchy::childrenOf(copy))
      children.push_back(child);
    CHECK(children.size() == 2u);
    if (children.size() == 2u)
    {
      const Entity copiedRig = children[0];
      const Entity copiedSign = children[1];
      const Entity copiedArm = childNamed(copiedRig, "Arm");
      const RigComponent* signRig = copiedSign.getComponent<RigComponent>();
      CHECK(signRig->arm == childNamed(copiedRig, "Blade A"));
      CHECK(signRig->blades[0] == childNamed(copiedArm, "Extra"));
      CHECK(signRig->blades[0] != extra);
      CHECK(signRig->blades[1] == copiedRig);
      CHECK(copiedArm.getComponent<RigComponent>()->arm == copiedSign);
    }
  }

  TEST("an entity added under a scene instance, and an instance added under "
       "one, should be named through their own scene ids")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    const Entity rig = w.serializer().instantiate("asset:rig", w.scene);
    Entity extra = w.scene.createEntity();
    extra.addComponent<NameComponent>("Extra");
    hierarchy::setParent(extra, rig);
    w.prefabs["asset:inner"] = rigPrefab();
    const Entity inner = w.serializer().instantiate(
        "asset:inner", w.scene, childNamed(rig, "Arm"));
    Entity pointer = w.scene.createEntity();
    pointer.addComponent<NameComponent>("Pointer");
    pointer.addComponent<RigComponent>(
        RigComponent{extra, {childNamed(inner, "Blade A"), inner}});
    w.scene.flushEntities();

    const json saved = w.serializer().save(w.scene);
    const json& added = saved["entities"][0]["$added"];
    // The walk meets the one under the arm first.
    CHECK(added.size() == 2u);
    const std::string innerId = added[0].value("$sceneId", json()).dump();
    const std::string extraId = added[1].value("$sceneId", json()).dump();
    CHECK(added[0].value("$prefab", "") == "asset:inner");
    CHECK(added[1].value("NameComponent", json::object()).value("name", "") ==
          "Extra");
    CHECK(saved["entities"][1]["RigComponent"] ==
          json({{"arm", "/" + extraId},
                {"blades", {"/" + innerId + "/2", "/" + innerId}}}));

    w.reload();
    const Entity rig2 = w.find("Rig");
    const Entity extra2 = w.find("Extra");
    const RigComponent* loaded = w.find("Pointer").getComponent<RigComponent>();
    CHECK(loaded->arm == extra2 && extra2.isValid());
    CHECK(hierarchy::parentOf(extra2) == rig2);
    const Entity inner2 = loaded->blades[1];
    CHECK(inner2.isValid() && inner2 != rig2 &&
          inner2.hasComponent<PrefabInstanceComponent>());
    CHECK(loaded->blades[0] == childNamed(inner2, "Blade A"));
    CHECK(w.serializer().save(w.scene) == saved);
  }

  TEST("an entity a prefab adds inside its nested instance should be named by "
       "its added id")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    Entity shed = w.scene.createEntity();
    shed.addComponent<NameComponent>("Shed");
    const Entity rig = w.serializer().instantiate("asset:rig", w.scene, shed);
    Entity lamp = w.scene.createEntity();
    lamp.addComponent<NameComponent>("Lamp");
    hierarchy::setParent(lamp, childNamed(rig, "Arm"));
    shed.addComponent<RigComponent>(RigComponent{lamp, {}});
    w.scene.flushEntities();

    const json saved = w.serializer().savePrefab(shed);
    const std::string arm = saved["root"]["RigComponent"]["arm"];
    const uint32_t rigId = rig.getComponent<PrefabLinkComponent>()->id;
    const uint32_t lampId = lamp.getComponent<PrefabLinkComponent>()->id;
    CHECK((lampId & kAddedNodeIdBit) != 0u);
    CHECK(arm == std::to_string(rigId) + "/" + std::to_string(lampId));

    World again;
    again.prefabs = w.prefabs;
    again.prefabs["asset:shed"] = saved;
    const Entity copy =
        again.serializer().instantiate("asset:shed", again.scene);
    const Entity copyRig =
        SceneSerializer::entityAtPath(copy, std::to_string(rigId));
    CHECK(copy.getComponent<RigComponent>()->arm ==
          childNamed(childNamed(copyRig, "Arm"), "Lamp"));
  }

  TEST("a '..' above the prefab's root should load as none and be saved back "
       "as written")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    json above = rigPrefab();
    above["root"]["RigComponent"] = {
        {"arm", "../../x"}, {"blades", json::array()}};
    w.prefabs["asset:above"] = above;
    w.prefabs["asset:yard"] = yardPrefab("../../9");

    Entity root = w.serializer().instantiate("asset:above", w.scene);
    CHECK(!root.getComponent<RigComponent>()->arm.isValid());
    CHECK(w.serializer().save(w.scene)["entities"][0] ==
          json({{"$prefab", "asset:above"}}));
    root.removeComponent<PrefabInstanceComponent>();
    CHECK(w.serializer().savePrefab(root, true)["root"]["RigComponent"] ==
          above["root"]["RigComponent"]);

    World yards;
    yards.prefabs = w.prefabs;
    Entity yard = yards.serializer().instantiate("asset:yard", yards.scene);
    CHECK(!SceneSerializer::entityAtPath(yard, "7")
               .getComponent<RigComponent>()
               ->arm.isValid());
    CHECK(yards.serializer().save(yards.scene)["entities"][0] ==
          json({{"$prefab", "asset:yard"}}));
    yard.removeComponent<PrefabInstanceComponent>();
    const json saved = yards.serializer().savePrefab(yard, true);
    CHECK(childEntry(saved["root"], 7) &&
          (*childEntry(saved["root"], 7))["$overrides"] ==
              json({{"", {{"RigComponent", {{"arm", "../../9"}}}}}}));
  }

  TEST("a part reference to a node not named as its part should be logged "
       "once; a missing target once per prefab and field however often "
       "it's made")
  {
    debugFeatures::allow(true);
    World w;
    json good = rigPrefab();
    good["root"]["PartRigComponent"] = {{"arm", "1"}};
    json stale = rigPrefab();
    stale["root"]["PartRigComponent"] = {{"arm", "2"}};
    json missing = rigPrefab();
    missing["root"]["RigComponent"] = {
        {"arm", "57"}, {"blades", json::array()}};
    w.prefabs["asset:good"] = good;
    w.prefabs["asset:stale"] = stale;
    w.prefabs["asset:missing"] = missing;

    LogLines log;
    for (int i = 0; i < 3; ++i)
    {
      const Entity made = w.serializer().instantiate("asset:good", w.scene);
      CHECK(made.getComponent<PartRigComponent>()->arm ==
            childNamed(made, "Arm"));
      w.serializer().instantiate("asset:stale", w.scene);
      w.serializer().instantiate("asset:missing", w.scene);
    }
    CHECK(log.count("isn't its part") == 1u);
    CHECK(log.count("asset:good") == 0u);
    CHECK(log.count("no entity at '57'") == 1u);
    CHECK(log.count("repeated") == 0u);
  }

  TEST("copying an instance whose prefab is missing should not copy its "
       "scene id; reading an instance's entry should give no ids")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    w.serializer().load(
        json{
            {"formatVersion", 3},
            {"entities",
             {{{"$prefab", "asset:gone"}, {"$sceneId", 5}},
              {{"NameComponent", {{"name", "Pointer"}}},
               {"RigComponent", {{"arm", "/5"}, {"blades", json::array()}}}}}}},
        w.scene);
    w.scene.flushEntities();
    Entity placeholder;
    for (const Entity& entity : w.scene.entities())
      if (entity.hasComponent<MissingPrefabComponent>())
        placeholder = entity;
    CHECK(w.find("Pointer").getComponent<RigComponent>()->arm == placeholder);
    const json copy = w.serializer().saveEntity(placeholder);
    CHECK(!copy.contains("$sceneId"));
    CHECK(copy["$prefab"] == "asset:gone");

    // A read of an instance whose part names a plain entity with no id yet.
    const Entity rig = w.serializer().instantiate("asset:rig", w.scene);
    Entity lamp = w.scene.createEntity();
    childNamed(rig, "Arm").addComponent<RigComponent>(RigComponent{lamp, {}});
    const json read = w.serializer().instanceEntry(rig);
    CHECK(!lamp.hasComponent<SceneIdComponent>());
    CHECK(read["$overrides"]["1"]["RigComponent"]["arm"].is_null());
    w.serializer().save(w.scene);
    CHECK(lamp.hasComponent<SceneIdComponent>());
  }

  TEST("an instance should have the prefab's entities, marked as its")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    const Entity root = w.serializer().instantiate("asset:fence", w.scene);
    CHECK(root.isValid());
    CHECK(root.hasComponent<PrefabInstanceComponent>());
    CHECK(root.getComponent<PrefabInstanceComponent>()->prefab ==
          "asset:fence");
    const Entity rail = w.find("Rail B");
    CHECK(rail.isValid());
    CHECK(hierarchy::parentOf(rail) == root);
    CHECK(rail.getComponent<PrefabLinkComponent>()->id == 2u);
    CHECK(w.tint("Rail B").g == 0.0f);
    CHECK(SceneSerializer::pathInInstance(rail, root) ==
          std::optional<std::string>("2"));
    CHECK(SceneSerializer::pathInInstance(root, root) ==
          std::optional<std::string>(""));
  }

  TEST("an unchanged instance should save as just the prefab")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    w.serializer().instantiate("asset:fence", w.scene);
    const json saved = w.serializer().save(w.scene);
    CHECK(saved["entities"].size() == 1u);
    CHECK(saved["entities"][0] == json({{"$prefab", "asset:fence"}}));
  }

  TEST("changes to an instance should save as overrides and load back")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    const Entity root = w.serializer().instantiate("asset:fence", w.scene);
    root.getComponent<TransformComponent>()->position = {5, 0, 0};
    w.find("Rail A").getComponent<MeshRendererComponent>()->tint = {0, 0, 1, 1};

    const json entry = w.serializer().save(w.scene)["entities"][0];
    CHECK(entry["$overrides"].size() == 2u);
    CHECK(entry["$overrides"][""]["TransformComponent"]["position"][0] == 5.0);
    CHECK(entry["$overrides"]["1"]["MeshRendererComponent"].size() ==
          1u); // just the tint
    CHECK(!entry["$overrides"].contains("2"));

    w.reload();
    CHECK(w.tint("Rail A").b == 1.0f);
    CHECK(w.find("Post").getComponent<TransformComponent>()->position.x ==
          5.0f);
    CHECK(w.find("Post").hasComponent<PrefabInstanceComponent>());
  }

  TEST("editing the prefab should update instances, keeping their overrides")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    w.serializer().instantiate("asset:fence", w.scene);
    w.find("Rail A").getComponent<MeshRendererComponent>()->tint = {0, 0, 1, 1};
    const json saved = w.serializer().save(w.scene);

    // Both rails turn green in the prefab.
    w.prefabs["asset:fence"]["root"]["children"][0]["MeshRendererComponent"]
             ["tint"] = {0, 1, 0, 1};
    w.prefabs["asset:fence"]["root"]["children"][1]["MeshRendererComponent"]
             ["tint"] = {0, 1, 0, 1};
    for (Entity entity : w.scene.entities())
      w.scene.destroyEntity(entity);
    w.scene.update(0.0);
    w.serializer().load(saved, w.scene);

    CHECK(w.tint("Rail A").b == 1.0f); // overridden: kept
    CHECK(w.tint("Rail B").g == 1.0f); // from the prefab: updated
  }

  TEST("removing and adding entities in an instance should round-trip")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    const Entity root = w.serializer().instantiate("asset:fence", w.scene);
    w.scene.destroyEntity(w.find("Rail B"));
    w.scene.update(0.0);
    Entity lamp = w.scene.createEntity();
    lamp.addComponent<NameComponent>("Lamp");
    lamp.addComponent<PointLightComponent>();
    hierarchy::setParent(lamp, root);

    const json entry = w.serializer().save(w.scene)["entities"][0];
    CHECK(entry["$removed"] == json::array({"2"}));
    CHECK(entry["$added"].size() == 1u);
    CHECK(entry["$added"][0]["$parent"] == "");

    w.reload();
    CHECK(!w.find("Rail B").isValid());
    CHECK(w.find("Lamp").isValid());
    CHECK(hierarchy::parentOf(w.find("Lamp")) == w.find("Post"));
    CHECK(!w.find("Lamp")
               .hasComponent<PrefabLinkComponent>()); // the scene's, not the
                                                      // prefab's
  }

  TEST("nested prefabs should expand, take overrides at any depth, and update")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    // A yard with a fence (rail A overridden yellow) as entity 5.
    json yard = named("Yard");
    yard["children"] = json::array(
        {{{"$id", 5},
          {"$prefab", "asset:fence"},
          {"$overrides",
           {{"1", {{"MeshRendererComponent", {{"tint", {1, 1, 0, 1}}}}}}}}}});
    w.prefabs["asset:yard"] = {{"formatVersion", 1}, {"root", yard}};

    const Entity root = w.serializer().instantiate("asset:yard", w.scene);
    const Entity fence = w.find("Post");
    CHECK(hierarchy::parentOf(fence) == root);
    CHECK(fence.hasComponent<PrefabInstanceComponent>());
    CHECK(fence.getComponent<PrefabLinkComponent>()->id == 5u);
    CHECK(w.tint("Rail A").b == 0.0f); // the yard's override
    CHECK(SceneSerializer::pathInInstance(w.find("Rail B"), root) ==
          std::optional<std::string>("5/2"));

    // The scene overrides rail B, deep inside.
    w.find("Rail B").getComponent<MeshRendererComponent>()->tint = {
        0.5f, 0.5f, 0.5f, 1};
    const json entry = w.serializer().save(w.scene)["entities"][0];
    CHECK(entry["$overrides"].size() == 1u);
    CHECK(entry["$overrides"].contains("5/2"));

    // The fence prefab changes rail A's mesh: still yellow, via the yard.
    w.prefabs["asset:fence"]["root"]["children"][0]["NameComponent"]["name"] =
        "Rail A2";
    for (Entity entity : w.scene.entities())
      w.scene.destroyEntity(entity);
    w.scene.update(0.0);
    w.serializer().load(
        json{{"formatVersion", 2}, {"entities", {entry}}}, w.scene);
    CHECK(w.find("Rail A2").isValid());
    CHECK(w.tint("Rail A2").r == 1.0f);
    CHECK(w.tint("Rail A2").b == 0.0f);
    CHECK(w.tint("Rail B").r == 0.5f);
  }

  TEST("a nested instance's own root should take overrides and added children")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    // A yard whose fence (entity 5) is moved and given a sign, by "" (its
    // root).
    json yard = named("Yard");
    yard["children"] = json::array(
        {{{"$id", 5},
          {"$prefab", "asset:fence"},
          {"$overrides",
           {{"", {{"TransformComponent", {{"position", {3, 0, 0}}}}}}}},
          {"$added", json::array({named("Sign", {{"$parent", ""}})})}}});
    w.prefabs["asset:yard"] = {{"formatVersion", 1}, {"root", yard}};

    w.serializer().instantiate("asset:yard", w.scene);
    const Entity fence = w.find("Post");
    CHECK(fence.getComponent<TransformComponent>()->position.x == 3.0f);
    CHECK(hierarchy::parentOf(w.find("Sign")) == fence);
  }

  TEST("an instance of a missing prefab should be kept as saved")
  {
    World w;
    const json entry = {
        {"$prefab", "asset:gone"},
        {"$overrides", {{"", {{"NameComponent", {{"name", "X"}}}}}}}};
    w.serializer().load(
        json{{"formatVersion", 2}, {"entities", {entry}}}, w.scene);
    CHECK(w.scene.entities().size() == 1u);
    CHECK(w.serializer().save(w.scene)["entities"][0] == entry);
  }

  TEST("a prefab containing itself should not expand forever")
  {
    World w;
    json loop = named("Loop");
    loop["children"] = json::array({{{"$id", 1}, {"$prefab", "asset:loop"}}});
    w.prefabs["asset:loop"] = {{"formatVersion", 1}, {"root", loop}};
    w.serializer().instantiate("asset:loop", w.scene);
    CHECK(w.scene.entities().size() < 40u);
  }

  TEST("a copy of a prefab added under a copy of itself should load: it's no "
       "cycle")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    LogLines log;
    const json entry = {
        {"$prefab", "asset:rig"},
        {"$added", {{{"$parent", "1"}, {"$prefab", "asset:rig"}}}}};
    w.serializer().load(
        json{{"formatVersion", 3}, {"entities", {entry}}}, w.scene);
    CHECK(log.count("contains itself") == 0u);
    w.scene.update(0.0);
    CHECK(w.scene.entities().size() == 8u);
    const Entity outer = w.find("Rig");
    const Entity inner = childNamed(childNamed(outer, "Arm"), "Rig");
    CHECK(inner.isValid());
    CHECK(inner.hasComponent<PrefabInstanceComponent>());
    // Each names its own parts.
    CHECK(inner.getComponent<RigComponent>()->arm == childNamed(inner, "Arm"));
    CHECK(w.serializer().save(w.scene)["entities"][0] == entry);
  }

  TEST("a prefab adding a copy of itself inside a nested instance should "
       "still be stopped as a cycle")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    json loop = named("Loop");
    loop["children"] = json::array(
        {{{"$id", 1},
          {"$prefab", "asset:rig"},
          {"$added", {{{"$parent", ""}, {"$prefab", "asset:loop"}}}}}});
    w.prefabs["asset:loop"] = {{"formatVersion", 2}, {"root", loop}};
    LogLines log;
    w.serializer().instantiate("asset:loop", w.scene);
    CHECK(log.count("contains itself") >= 1u);
    CHECK(w.scene.entities().size() < 40u);
  }

  TEST("a prefab's content open on its own should keep its prefab ids through "
       "a scene save and load (an editor undo), so it saves as before")
  {
    World w;
    w.prefabs["asset:rig"] = rigPrefab();
    w.prefabs["asset:yard"] = yardPrefab("2");
    Entity yard = w.serializer().instantiate("asset:yard", w.scene);
    yard.removeComponent<PrefabInstanceComponent>(); // as the editor opens it
    w.scene.flushEntities();
    const json before = w.serializer().savePrefab(yard, true);
    // Its ids aren't 1, 2, 3 in order, so numbering them again would show.
    CHECK(childEntry(before["root"], 3) != nullptr);
    CHECK(childEntry(before["root"], 7) != nullptr);

    // An ordinary scene save writes no prefab ids.
    CHECK(!w.serializer().save(w.scene)["entities"][0]["children"][1].contains(
        "$id"));
    const json snapshot = w.serializer().save(w.scene, {}, true);
    CHECK(snapshot["entities"][0]["children"][1]["$id"] == 3);
    for (Entity entity : w.scene.entities())
      w.scene.destroyEntity(entity);
    w.scene.update(0.0);
    w.serializer().load(snapshot, w.scene);
    const Entity again = w.find("Yard");
    CHECK(again.isValid());
    CHECK(childNamed(again, "Post").getComponent<PrefabLinkComponent>()->id ==
          3u);
    CHECK(w.serializer().savePrefab(again, true) == before);
  }

  TEST("expanding a prefab should list its entities, nested ones and overrides "
       "included, making none")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    json yard = named("Yard");
    yard["children"] = json::array(
        {{{"$id", 5},
          {"$prefab", "asset:fence"},
          {"$overrides",
           {{"1", {{"MeshRendererComponent", {{"tint", {1, 1, 0, 1}}}}}}}}}});
    w.prefabs["asset:yard"] = {{"formatVersion", 1}, {"root", yard}};

    const auto entities = w.serializer().expandPrefab("asset:yard");
    w.scene.update(0.0);
    CHECK(w.scene.entities().empty());
    CHECK(entities.size() ==
          1u + w.serializer().expandPrefab("asset:fence").size());
    CHECK(entities.front().key.empty()); // the root first
    const auto railA =
        std::find_if(entities.begin(),
                     entities.end(),
                     [](const auto& e) { return e.key == "5/1"; });
    CHECK(railA != entities.end());
    CHECK(railA != entities.end() && railA->parent == "5");
    CHECK(railA != entities.end() &&
          railA->components["MeshRendererComponent"]["tint"][2] ==
              0); // overridden
    CHECK(w.serializer().expandPrefab("asset:nothing").empty());
  }

  TEST("saving a prefab should give ids, keep nested instances and zero its "
       "place")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    Entity barn = w.scene.createEntity();
    barn.addComponent<NameComponent>("Barn");
    barn.addComponent<TransformComponent>(TransformComponent{{3, 4, 0}});
    Entity door = w.scene.createEntity();
    door.addComponent<NameComponent>("Door");
    hierarchy::setParent(door, barn);
    const Entity fence =
        w.serializer().instantiate("asset:fence", w.scene, barn);
    w.find("Rail A").getComponent<MeshRendererComponent>()->tint = {0, 1, 1, 1};

    const json prefab = w.serializer().savePrefab(barn);
    const json& root = prefab["root"];
    CHECK(root["TransformComponent"]["position"] ==
          json::array({0.0, 0.0, 0.0}));
    CHECK(root["children"].size() == 2u);
    CHECK(root["children"][0].contains("$id"));
    CHECK(root["children"][1]["$prefab"] == "asset:fence");
    CHECK(root["children"][1]["$overrides"].contains("1"));
    CHECK(door.hasComponent<PrefabLinkComponent>());
    CHECK(fence.hasComponent<PrefabLinkComponent>());
    CHECK(door.getComponent<PrefabLinkComponent>()->id !=
          fence.getComponent<PrefabLinkComponent>()->id);

    // It loads as a prefab of its own.
    w.prefabs["asset:barn"] = prefab;
    for (Entity entity : w.scene.entities())
      w.scene.destroyEntity(entity);
    w.scene.update(0.0);
    w.serializer().instantiate("asset:barn", w.scene);
    CHECK(w.find("Door").isValid());
    CHECK(w.tint("Rail A").g == 1.0f);
  }

  TEST("applying an instance should keep the prefab's own root placement")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    const Entity root = w.serializer().instantiate("asset:fence", w.scene);
    root.getComponent<TransformComponent>()->position = {9, 9, 0};
    w.find("Rail A").getComponent<MeshRendererComponent>()->tint = {0, 0, 0, 1};
    const json applied = w.serializer().savePrefab(root);
    CHECK(applied["root"]["TransformComponent"]["position"] ==
          json::array({0, 0, 0}));
    CHECK(applied["root"]["children"][0]["MeshRendererComponent"]["tint"][0] ==
          0.0);
    CHECK(applied["root"]["children"][0]["$id"] == 1);
  }

  TEST("a model's entities should record their rest pose after overrides; a "
       "prefab's own should not")
  {
    World w;
    json arm = named(
        "Arm", {{"$id", 1}, {"TransformComponent", {{"position", {0, 0, 1}}}}});
    arm["children"] = json::array({named(
        "Joint",
        {{"$id", 2}, {"TransformComponent", {{"position", {0, 0, 2}}}}})});
    json model = named("Model", {{"TransformComponent", json::object()}});
    model["children"] = json::array({arm});
    w.prefabs["asset:model"] = {
        {"formatVersion", 1}, {"model", true}, {"root", model}};
    json cart = named("Cart", {{"TransformComponent", json::object()}});
    cart["children"] = json::array(
        {named("Handle",
               {{"$id", 1}, {"TransformComponent", {{"position", {1, 0, 0}}}}}),
         {{"$id", 2}, {"$prefab", "asset:model"}}});
    w.prefabs["asset:cart"] = {{"formatVersion", 1}, {"root", cart}};

    const json entry = {
        {"$prefab", "asset:cart"},
        {"$overrides",
         {{"2/2", {{"TransformComponent", {{"position", {0, 0, 7}}}}}}}}};
    w.serializer().load({{"formatVersion", 2}, {"entities", {entry}}}, w.scene);

    const Entity joint = w.find("Joint");
    CHECK(joint.hasComponent<RestPoseComponent>());
    CHECK(joint.getComponent<RestPoseComponent>()->position ==
          glm::vec3(0, 0, 7));
    CHECK(w.find("Arm").getComponent<RestPoseComponent>()->position ==
          glm::vec3(0, 0, 1));
    CHECK(w.find("Model").hasComponent<RestPoseComponent>());
    CHECK(!w.find("Cart").hasComponent<RestPoseComponent>());
    CHECK(!w.find("Handle").hasComponent<RestPoseComponent>());

    // Recorded, not saved: the instance saves as it was loaded.
    joint.getComponent<TransformComponent>()->position = {0, 0, 7};
    const json saved = w.serializer().save(w.scene);
    CHECK(saved["entities"].size() == 1u);
    CHECK(saved["entities"][0] == entry);
  }

  TEST("a prefab with model nodes' old ids should expand as its upgraded form, "
       "nested in a scene's instance too")
  {
    World w;
    w.prefabs["asset:model"] = stableModel();
    // Old ids are the nodes' places in the model's order: Arm 1, Hand 2, Leg 3.
    const json old = cartWithModel(
        {{"1", {{"PointLightComponent", json::object()}}}}, {"3"}, "2");
    const json upgraded = cartWithModel(
        {{kArm, {{"PointLightComponent", json::object()}}}}, {kLeg}, kHand);
    w.prefabs["asset:old"] = old;
    w.prefabs["asset:new"] = upgraded;
    const SceneSerializer serializer = w.serializer();
    const auto fromOld = serializer.expandPrefab("asset:old");
    const auto fromNew = serializer.expandPrefab("asset:new");
    CHECK(fromOld.size() == 5u); // cart, model, arm, hand, glove
    CHECK(fromOld.size() == fromNew.size());
    for (std::size_t i = 0; i < std::min(fromOld.size(), fromNew.size()); ++i)
    {
      CHECK(fromOld[i].key == fromNew[i].key);
      CHECK(fromOld[i].parent == fromNew[i].parent);
      CHECK(fromOld[i].components == fromNew[i].components);
    }
    // A scene's override through the cart into the model, both ways.
    const json oldEntry = {
        {"$prefab", "asset:new"},
        {"$overrides", {{"7/2", {{"NameComponent", {{"name", "Paw"}}}}}}}};
    json newEntry = oldEntry;
    newEntry["$overrides"] = {
        {std::string("7/") + kHand, {{"NameComponent", {{"name", "Paw"}}}}}};
    const auto a = serializer.expandEntry(oldEntry);
    const auto b = serializer.expandEntry(newEntry);
    CHECK(a.size() == b.size());
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
      CHECK(a[i].components == b[i].components);
    CHECK(w.serializer().load(
        {{"formatVersion", 2}, {"entities", {oldEntry}}}, w.scene));
    CHECK(w.find("Paw").isValid());
    CHECK(w.find("Paw").getComponent<PrefabLinkComponent>()->id ==
          std::stoul(kHand));
    CHECK(hierarchy::parentOf(w.find("Glove")) == w.find("Paw"));
    CHECK(w.find("Arm").hasComponent<PointLightComponent>());
    CHECK(!w.find("Leg").isValid());
  }

  TEST("changes naming nodes a model no longer has should be kept, warned once "
       "and saved back unchanged")
  {
    World w;
    w.prefabs["asset:model"] = stableModel();
    w.prefabs["asset:cart"] = cartWithModel(json::object(), {}, "");
    const json gone = {{"PointLightComponent", json::object()}};
    const json entry = {
        {"$prefab", "asset:cart"},
        {"$overrides",
         {{std::string("7/") + kArm, {{"NameComponent", {{"name", "Limb"}}}}},
          {"7/2999999999", gone}}},
        {"$removed", {"7/3111111111"}},
        {"$added", {named("Ghost", {{"$parent", "7/3222222222"}})}}};
    CHECK(w.serializer().load(
        {{"formatVersion", 3}, {"entities", {entry}}}, w.scene));
    const Entity cart = w.find("Cart");
    CHECK(w.find("Limb").isValid());
    CHECK(!w.find("Ghost").isValid());
    CHECK(cart.hasComponent<UnresolvedComponents>());
    if (UnresolvedComponents* unresolvedComponents =
            cart.getComponent<UnresolvedComponents>())
    {
      const json& unused = unresolvedComponents->overrides;
      CHECK(unused["$overrides"] == json({{"7/2999999999", gone}}));
      CHECK(unused["$removed"] == json::array({"7/3111111111"}));
      CHECK(unused["$added"][0]["$parent"] == "7/3222222222");
      CHECK(unresolvedComponents->components.empty());
    }
    const json saved = w.serializer().save(w.scene);
    CHECK(saved["entities"].size() == 1u);
    CHECK(saved["entities"][0] == entry);

    // An old id past the model's nodes is kept as "old:<id>", so it can never
    // name a node the model gains later.
    World old;
    old.prefabs["asset:model"] = stableModel();
    old.prefabs["asset:cart"] = cartWithModel(json::object(), {}, "");
    const json oldEntry = {
        {"$prefab", "asset:cart"}, {"$overrides", {{"7/9", gone}}}};
    old.serializer().load(
        {{"formatVersion", 2}, {"entities", {oldEntry}}}, old.scene);
    const json kept = {
        {"$prefab", "asset:cart"}, {"$overrides", {{"7/old:9", gone}}}};
    CHECK(old.serializer().save(old.scene)["entities"][0] == kept);
    CHECK(old.serializer()
              .nodeIdEdits({{"formatVersion", 3}, {"entities", {kept}}})
              .empty());
  }

  TEST("unused removals and additions should be saved back in their places")
  {
    World w;
    w.prefabs["asset:model"] = stableModel();
    w.prefabs["asset:cart"] = cartWithModel(json::object(), {}, "");
    const json entry = {
        {"$prefab", "asset:cart"},
        {"$removed", {"7/3111111111", std::string("7/") + kLeg}},
        {"$added",
         {named("Ghost", {{"$parent", "7/3222222222"}}),
          named("Bell", {{"$parent", std::string("7/") + kArm}})}}};
    w.serializer().load({{"formatVersion", 3}, {"entities", {entry}}}, w.scene);
    CHECK(w.find("Bell").isValid());
    CHECK(!w.find("Leg").isValid());
    CHECK(w.serializer().save(w.scene)["entities"][0] == entry);
  }

  TEST("what a prefab adds inside its nested instance should be the prefab's "
       "own in every instance of it")
  {
    World w;
    w.prefabs["asset:model"] = stableModel();
    // The cart adds a glove under the model's hand (with no id, as files from
    // before had them).
    w.prefabs["asset:cart"] = cartWithModel(json::object(), {}, kHand);
    const Entity cart = w.serializer().instantiate("asset:cart", w.scene);
    const Entity glove = w.find("Glove");
    CHECK(glove.hasComponent<PrefabLinkComponent>());
    const PrefabLinkComponent* link = glove.getComponent<PrefabLinkComponent>();
    const uint32_t id = link ? link->id : 0;
    CHECK((id & kAddedNodeIdBit) != 0u);
    CHECK((id & kModelNodeIdBit) == 0u);
    const std::string path = "7/" + std::to_string(id);
    CHECK(SceneSerializer::pathInInstance(glove, cart) ==
          std::optional<std::string>(path));

    // Unchanged, the instance saves as just the prefab; changed, as an
    // override that loads back onto it.
    CHECK(w.serializer().save(w.scene)["entities"][0] ==
          json({{"$prefab", "asset:cart"}}));
    glove.getComponent<NameComponent>()->name = "Mitten";
    const json saved = w.serializer().save(w.scene)["entities"][0];
    const json mitten = {{"NameComponent", {{"name", "Mitten"}}}};
    CHECK(saved ==
          json({{"$prefab", "asset:cart"}, {"$overrides", {{path, mitten}}}}));
    World again;
    again.prefabs = w.prefabs;
    again.serializer().load(
        {{"formatVersion", 3}, {"entities", {saved}}}, again.scene);
    CHECK(again.find("Mitten").isValid());
    CHECK(hierarchy::parentOf(again.find("Mitten")) == again.find("Hand"));

    // Saved as the prefab (Apply), the glove keeps its id, and a part added
    // beside it gets another.
    glove.getComponent<NameComponent>()->name = "Glove";
    Entity strap = w.scene.createEntity();
    strap.addComponent<NameComponent>("Strap");
    hierarchy::setParent(strap, w.find("Hand"));
    w.scene.flushEntities();
    const json applied = w.serializer().savePrefab(cart);
    const json& added = applied["root"]["children"][0]["$added"];
    CHECK(added.size() == 2u);
    CHECK(added[0]["$id"] == id);
    CHECK(added[1].contains("$id"));
    CHECK(added[1].value("$id", 0u) != id);
    CHECK((added[1].value("$id", 0u) & kAddedNodeIdBit) != 0u);
    w.prefabs["asset:cart"] = applied;
    World applies;
    applies.prefabs = w.prefabs;
    const Entity copy =
        applies.serializer().instantiate("asset:cart", applies.scene);
    CHECK(applies.find("Glove").getComponent<PrefabLinkComponent>()->id == id);
    CHECK(applies.serializer().save(applies.scene)["entities"][0] ==
          json({{"$prefab", "asset:cart"}}));
    CHECK(SceneSerializer::pathInInstance(applies.find("Strap"), copy)
              .has_value());
  }

  TEST("an instance whose prefab can't be read, or is named by a non-string, "
       "should be reported and kept")
  {
    World w;
    const json file = {
        {"formatVersion", 3},
        {"entities", {{{"$prefab", "asset:gone"}}, {{"$prefab", 5}}}}};
    std::vector<std::string> unreadable;
    CHECK(w.serializer().nodeIdEdits(file, &unreadable).empty());
    CHECK(unreadable == std::vector<std::string>({"asset:gone", "5"}));
    CHECK(w.serializer().load(file, w.scene));
    CHECK(w.serializer().save(w.scene)["entities"] == file["entities"]);
  }

  TEST("an unpacked model node's stable id should be kept, and new ids count "
       "on from the prefab's own")
  {
    World w;
    Entity root = w.scene.createEntity();
    root.addComponent<NameComponent>("Root");
    Entity node = w.scene.createEntity();
    node.addComponent<NameComponent>("Node");
    node.addComponent<PrefabLinkComponent>(
        PrefabLinkComponent{static_cast<uint32_t>(std::stoul(kArm))});
    hierarchy::setParent(node, root);
    Entity own = w.scene.createEntity();
    own.addComponent<NameComponent>("Own");
    own.addComponent<PrefabLinkComponent>(PrefabLinkComponent{4});
    hierarchy::setParent(own, root);
    Entity fresh = w.scene.createEntity();
    fresh.addComponent<NameComponent>("Fresh");
    hierarchy::setParent(fresh, root);
    w.scene.flushEntities();

    const json saved = w.serializer().savePrefab(root);
    CHECK(saved["formatVersion"] == SceneSerializer::kPrefabFormatVersion);
    CHECK(saved["root"]["children"][0]["$id"] == std::stoul(kArm));
    CHECK(saved["root"]["children"][1]["$id"] == 4);
    CHECK(saved["root"]["children"][2]["$id"] == 5);
  }

  return testing::report("prefabTests");
}
