#include "../../testHarness.h"
#include "../rendering3d/mockRenderer3D.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/pointLight3DComponent.h>
#include <engine/core/components/transform3DComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/rendering/3d/meshPrimitives.h>
#include <engine/core/reflection/reflection.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering3d/renderSystem3D.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/serialization/prefabComponents.h>
#include <engine/runtime/serialization/sceneSerializer.h>

#include <algorithm>
#include <map>
#include <string>

using namespace sfs;
using sfs::testing3d::MockRenderer3D;
using json = nlohmann::json;

namespace
{
// A scene, plus prefab files in memory.
struct World
{
  AssetStore store;
  MockRenderer3D renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer3D = &renderer}};
  RenderSystem3D& render = scene.addSystem<RenderSystem3D>(renderer);
  MeshHandle cube = render.meshes().add(meshes::box({1, 1, 1}), "builtin:cube");
  std::map<std::string, json> prefabs;

  SceneSerializer serializer()
  {
    SceneSerializer out(reflect::types(), &render.meshes());
    out.setPrefabSource([this](std::string_view name) -> std::optional<json>
                        {
                          const auto it = prefabs.find(std::string(name));
                          return it == prefabs.end() ? std::nullopt : std::optional<json>(it->second);
                        });
    return out;
  }

  Entity find(const std::string& name)
  {
    scene.update(0.0); // new entities are listed once the scene updates
    for (Entity entity : scene.entities())
      if (entity.hasComponent<NameComponent>() && entity.getComponent<NameComponent>().name == name)
        return entity;
    return {};
  }

  glm::vec4 tint(const std::string& name) { return find(name).getComponent<MeshRendererComponent>().tint; }

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

// A fence: a post with two rails, rail 2 red.
json fencePrefab()
{
  json post = named("Post", {{"Transform3DComponent", {{"position", {0, 0, 0}}}}});
  post["children"] = json::array(
      {named("Rail A", {{"$id", 1}, {"MeshRendererComponent", {{"mesh", "builtin:cube"}, {"tint", {1, 1, 1, 1}}}}}),
       named("Rail B", {{"$id", 2}, {"MeshRendererComponent", {{"mesh", "builtin:cube"}, {"tint", {1, 0, 0, 1}}}}})});
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
    CHECK(SceneSerializer::pathInInstance(rail, root) == std::optional<std::string>("2"));
    CHECK(SceneSerializer::pathInInstance(root, root) == std::optional<std::string>(""));
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
    root.getComponent<Transform3DComponent>().position = {5, 0, 0};
    w.find("Rail A").getComponent<MeshRendererComponent>().tint = {0, 0, 1, 1};

    const json entry = w.serializer().save(w.scene)["entities"][0];
    CHECK(entry["$overrides"].size() == 2u);
    CHECK(entry["$overrides"][""]["Transform3DComponent"]["position"][0] == 5.0);
    CHECK(entry["$overrides"]["1"]["MeshRendererComponent"].size() == 1u); // just the tint
    CHECK(!entry["$overrides"].contains("2"));

    w.reload();
    CHECK(w.tint("Rail A").b == 1.0f);
    CHECK(w.find("Post").getComponent<Transform3DComponent>().position.x == 5.0f);
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
    w.prefabs["asset:fence"]["root"]["children"][0]["MeshRendererComponent"]["tint"] = {0, 1, 0, 1};
    w.prefabs["asset:fence"]["root"]["children"][1]["MeshRendererComponent"]["tint"] = {0, 1, 0, 1};
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
    lamp.addComponent<PointLight3DComponent>();
    hierarchy::setParent(lamp, root);

    const json entry = w.serializer().save(w.scene)["entities"][0];
    CHECK(entry["$removed"] == json::array({"2"}));
    CHECK(entry["$added"].size() == 1u);
    CHECK(entry["$added"][0]["$parent"] == "");

    w.reload();
    CHECK(!w.find("Rail B").isValid());
    CHECK(w.find("Lamp").isValid());
    CHECK(hierarchy::parentOf(w.find("Lamp")) == w.find("Post"));
    CHECK(!w.find("Lamp").hasComponent<PrefabLinkComponent>()); // the scene's, not the prefab's
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
          {"$overrides", {{"1", {{"MeshRendererComponent", {{"tint", {1, 1, 0, 1}}}}}}}}}});
    w.prefabs["asset:yard"] = {{"formatVersion", 1}, {"root", yard}};

    const Entity root = w.serializer().instantiate("asset:yard", w.scene);
    const Entity fence = w.find("Post");
    CHECK(hierarchy::parentOf(fence) == root);
    CHECK(fence.hasComponent<PrefabInstanceComponent>());
    CHECK(fence.getComponent<PrefabLinkComponent>().id == 5u);
    CHECK(w.tint("Rail A").b == 0.0f); // the yard's override
    CHECK(SceneSerializer::pathInInstance(w.find("Rail B"), root) == std::optional<std::string>("5/2"));

    // The scene overrides rail B, deep inside.
    w.find("Rail B").getComponent<MeshRendererComponent>().tint = {0.5f, 0.5f, 0.5f, 1};
    const json entry = w.serializer().save(w.scene)["entities"][0];
    CHECK(entry["$overrides"].size() == 1u);
    CHECK(entry["$overrides"].contains("5/2"));

    // The fence prefab changes rail A's mesh: still yellow, via the yard.
    w.prefabs["asset:fence"]["root"]["children"][0]["NameComponent"]["name"] = "Rail A2";
    for (Entity entity : w.scene.entities())
      w.scene.destroyEntity(entity);
    w.scene.update(0.0);
    w.serializer().load(json{{"formatVersion", 2}, {"entities", {entry}}}, w.scene);
    CHECK(w.find("Rail A2").isValid());
    CHECK(w.tint("Rail A2").r == 1.0f);
    CHECK(w.tint("Rail A2").b == 0.0f);
    CHECK(w.tint("Rail B").r == 0.5f);
  }

  TEST("an instance of a missing prefab should be kept as saved")
  {
    World w;
    const json entry = {{"$prefab", "asset:gone"}, {"$overrides", {{"", {{"NameComponent", {{"name", "X"}}}}}}}};
    w.serializer().load(json{{"formatVersion", 2}, {"entities", {entry}}}, w.scene);
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

  TEST("expanding a prefab should list its entities, nested ones and overrides included, making none")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    json yard = named("Yard");
    yard["children"] = json::array(
        {{{"$id", 5},
          {"$prefab", "asset:fence"},
          {"$overrides", {{"1", {{"MeshRendererComponent", {{"tint", {1, 1, 0, 1}}}}}}}}}});
    w.prefabs["asset:yard"] = {{"formatVersion", 1}, {"root", yard}};

    const auto entities = w.serializer().expandPrefab("asset:yard");
    w.scene.update(0.0);
    CHECK(w.scene.entities().empty());
    CHECK(entities.size() == 1u + w.serializer().expandPrefab("asset:fence").size());
    CHECK(entities.front().key.empty()); // the root first
    const auto railA = std::find_if(entities.begin(), entities.end(),
                                    [](const auto& e) { return e.key == "5/1"; });
    CHECK(railA != entities.end());
    CHECK(railA != entities.end() && railA->parent == "5");
    CHECK(railA != entities.end() && railA->components["MeshRendererComponent"]["tint"][2] == 0); // overridden
    CHECK(w.serializer().expandPrefab("asset:nothing").empty());
  }

  TEST("saving a prefab should give ids, keep nested instances and zero its place")
  {
    World w;
    w.prefabs["asset:fence"] = fencePrefab();
    Entity barn = w.scene.createEntity();
    barn.addComponent<NameComponent>("Barn");
    barn.addComponent<Transform3DComponent>(Transform3DComponent{{3, 4, 0}});
    Entity door = w.scene.createEntity();
    door.addComponent<NameComponent>("Door");
    hierarchy::setParent(door, barn);
    const Entity fence = w.serializer().instantiate("asset:fence", w.scene, barn);
    w.find("Rail A").getComponent<MeshRendererComponent>().tint = {0, 1, 1, 1};

    const json prefab = w.serializer().savePrefab(w.scene, barn);
    const json& root = prefab["root"];
    CHECK(root["Transform3DComponent"]["position"] == json::array({0.0, 0.0, 0.0}));
    CHECK(root["children"].size() == 2u);
    CHECK(root["children"][0].contains("$id"));
    CHECK(root["children"][1]["$prefab"] == "asset:fence");
    CHECK(root["children"][1]["$overrides"].contains("1"));
    CHECK(door.hasComponent<PrefabLinkComponent>());
    CHECK(fence.hasComponent<PrefabLinkComponent>());
    CHECK(door.getComponent<PrefabLinkComponent>().id != fence.getComponent<PrefabLinkComponent>().id);

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
    root.getComponent<Transform3DComponent>().position = {9, 9, 0};
    w.find("Rail A").getComponent<MeshRendererComponent>().tint = {0, 0, 0, 1};
    const json applied = w.serializer().savePrefab(w.scene, root);
    CHECK(applied["root"]["Transform3DComponent"]["position"] == json::array({0, 0, 0}));
    CHECK(applied["root"]["children"][0]["MeshRendererComponent"]["tint"][0] == 0.0);
    CHECK(applied["root"]["children"][0]["$id"] == 1);
  }

  return testing::report("prefabTests");
}
