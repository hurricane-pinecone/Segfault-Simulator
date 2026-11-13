#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/pointLightComponent.h>
#include <engine/core/components/restPoseComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/reflection/reflection.h>
#include <engine/core/rendering/meshPrimitives.h>
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
using sfs::testing3d::MockRenderer;
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
      if (entity.hasComponent<NameComponent>() &&
          entity.getComponent<NameComponent>().name == name)
        return entity;
    return {};
  }

  glm::vec4 tint(const std::string& name)
  {
    return find(name).getComponent<MeshRendererComponent>().tint;
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
} // namespace

int main()
{
  TEST("an instance should have the prefab's entities, marked as its")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    const Entity root = w.serializer().instantiate("asset:fence", w.scene);
    CHECK(root.isValid());
    CHECK(root.hasComponent<PrefabInstanceComponent>());
    CHECK(root.getComponent<PrefabInstanceComponent>().prefab == "asset:fence");
    const Entity rail = w.find("Rail B");
    CHECK(rail.isValid());
    CHECK(hierarchy::parentOf(rail) == root);
    CHECK(rail.getComponent<PrefabLinkComponent>().id == 2u);
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
    root.getComponent<TransformComponent>().position = {5, 0, 0};
    w.find("Rail A").getComponent<MeshRendererComponent>().tint = {0, 0, 1, 1};

    const json entry = w.serializer().save(w.scene)["entities"][0];
    CHECK(entry["$overrides"].size() == 2u);
    CHECK(entry["$overrides"][""]["TransformComponent"]["position"][0] == 5.0);
    CHECK(entry["$overrides"]["1"]["MeshRendererComponent"].size() ==
          1u); // just the tint
    CHECK(!entry["$overrides"].contains("2"));

    w.reload();
    CHECK(w.tint("Rail A").b == 1.0f);
    CHECK(w.find("Post").getComponent<TransformComponent>().position.x == 5.0f);
    CHECK(w.find("Post").hasComponent<PrefabInstanceComponent>());
  }

  TEST("editing the prefab should update instances, keeping their overrides")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    w.serializer().instantiate("asset:fence", w.scene);
    w.find("Rail A").getComponent<MeshRendererComponent>().tint = {0, 0, 1, 1};
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
    CHECK(fence.getComponent<PrefabLinkComponent>().id == 5u);
    CHECK(w.tint("Rail A").b == 0.0f); // the yard's override
    CHECK(SceneSerializer::pathInInstance(w.find("Rail B"), root) ==
          std::optional<std::string>("5/2"));

    // The scene overrides rail B, deep inside.
    w.find("Rail B").getComponent<MeshRendererComponent>().tint = {
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
    CHECK(fence.getComponent<TransformComponent>().position.x == 3.0f);
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
    w.find("Rail A").getComponent<MeshRendererComponent>().tint = {0, 1, 1, 1};

    const json prefab = w.serializer().savePrefab(w.scene, barn);
    const json& root = prefab["root"];
    CHECK(root["TransformComponent"]["position"] ==
          json::array({0.0, 0.0, 0.0}));
    CHECK(root["children"].size() == 2u);
    CHECK(root["children"][0].contains("$id"));
    CHECK(root["children"][1]["$prefab"] == "asset:fence");
    CHECK(root["children"][1]["$overrides"].contains("1"));
    CHECK(door.hasComponent<PrefabLinkComponent>());
    CHECK(fence.hasComponent<PrefabLinkComponent>());
    CHECK(door.getComponent<PrefabLinkComponent>().id !=
          fence.getComponent<PrefabLinkComponent>().id);

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
    root.getComponent<TransformComponent>().position = {9, 9, 0};
    w.find("Rail A").getComponent<MeshRendererComponent>().tint = {0, 0, 0, 1};
    const json applied = w.serializer().savePrefab(w.scene, root);
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
    CHECK(joint.getComponent<RestPoseComponent>().position ==
          glm::vec3(0, 0, 7));
    CHECK(w.find("Arm").getComponent<RestPoseComponent>().position ==
          glm::vec3(0, 0, 1));
    CHECK(w.find("Model").hasComponent<RestPoseComponent>());
    CHECK(!w.find("Cart").hasComponent<RestPoseComponent>());
    CHECK(!w.find("Handle").hasComponent<RestPoseComponent>());

    // Recorded, not saved: the instance saves as it was loaded.
    joint.getComponent<TransformComponent>().position = {0, 0, 7};
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
    CHECK(w.find("Paw").getComponent<PrefabLinkComponent>().id ==
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
    if (cart.hasComponent<UnresolvedComponents>())
    {
      const json& unused = cart.getComponent<UnresolvedComponents>().overrides;
      CHECK(unused["$overrides"] == json({{"7/2999999999", gone}}));
      CHECK(unused["$removed"] == json::array({"7/3111111111"}));
      CHECK(unused["$added"][0]["$parent"] == "7/3222222222");
      CHECK(cart.getComponent<UnresolvedComponents>().components.empty());
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
    const uint32_t id = glove.hasComponent<PrefabLinkComponent>()
                            ? glove.getComponent<PrefabLinkComponent>().id
                            : 0;
    CHECK((id & kAddedNodeIdBit) != 0u);
    CHECK((id & kModelNodeIdBit) == 0u);
    const std::string path = "7/" + std::to_string(id);
    CHECK(SceneSerializer::pathInInstance(glove, cart) ==
          std::optional<std::string>(path));

    // Unchanged, the instance saves as just the prefab; changed, as an
    // override that loads back onto it.
    CHECK(w.serializer().save(w.scene)["entities"][0] ==
          json({{"$prefab", "asset:cart"}}));
    glove.getComponent<NameComponent>().name = "Mitten";
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
    glove.getComponent<NameComponent>().name = "Glove";
    Entity strap = w.scene.createEntity();
    strap.addComponent<NameComponent>("Strap");
    hierarchy::setParent(strap, w.find("Hand"));
    w.scene.flushEntities();
    const json applied = w.serializer().savePrefab(w.scene, cart);
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
    CHECK(applies.find("Glove").getComponent<PrefabLinkComponent>().id == id);
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

    const json saved = w.serializer().savePrefab(w.scene, root);
    CHECK(saved["formatVersion"] == SceneSerializer::kPrefabFormatVersion);
    CHECK(saved["root"]["children"][0]["$id"] == std::stoul(kArm));
    CHECK(saved["root"]["children"][1]["$id"] == 4);
    CHECK(saved["root"]["children"][2]["$id"] == 5);
  }

  return testing::report("prefabTests");
}
