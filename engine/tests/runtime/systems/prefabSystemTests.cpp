#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/core/components/capsuleCollider3DComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/scriptComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/game/gameObject.h>
#include <engine/runtime/rendering/standardScene.h>
#include <engine/runtime/sceneManager/sceneManager.h>
#include <engine/runtime/scripting/scriptRegistry.h>
#include <engine/runtime/systems/physicsSystem3D.h>
#include <engine/runtime/systems/prefabSystem.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace sfs;
namespace fs = std::filesystem;

namespace
{
// Counts the objects made (onCreate) from its script.
class Counter : public GameObject
{
public:
  static inline int created = 0;
  void onCreate(Scene&) override { ++created; }
};

// A project with two prefabs: a tree (a capsule trunk 0.5 m round, 4 m tall,
// a scripted child) and a stand-alone post.
struct Project
{
  fs::path folder = fs::temp_directory_path() / "sfs_prefab_system_test";
  std::string tree, post;

  Project()
  {
    fs::remove_all(folder);
    fs::create_directories(folder / "prefabs");
    std::ofstream(folder / "prefabs" / "tree.prefab") << R"({
      "formatVersion": 1,
      "root": {
        "NameComponent": {"name": "Tree"},
        "TransformComponent": {"position": [0, 0, 0]},
        "CapsuleCollider3DComponent": {"radius": 0.5, "height": 4.0,
                                       "offset": [0, 0, 2]},
        "ScriptComponent": {"script": "Counter"},
        "children": [{"$id": 1, "NameComponent": {"name": "Crown"},
                      "TransformComponent": {"position": [0, 0, 5]},
                      "ScriptComponent": {"script": "Counter"}}]}})";
    std::ofstream(folder / "prefabs" / "post.prefab") << R"({
      "formatVersion": 1,
      "root": {"NameComponent": {"name": "Post"}}})";
    const AssetDatabase database(folder,
                                 AssetDatabase::Mode::ReadWrite,
                                 AssetDatabase::projectFolders());
    if (const AssetRecord* r =
            database.findByPath(fs::path("prefabs") / "tree.prefab"))
      tree = "asset:" + r->id.toString();
    if (const AssetRecord* r =
            database.findByPath(fs::path("prefabs") / "post.prefab"))
      post = "asset:" + r->id.toString();
  }
  ~Project() { fs::remove_all(folder); }
};

// A running scene on the project, with physics.
struct World
{
  AssetDatabase database;
  AssetStore store;
  testing3d::MockRenderer renderer;
  SceneServices services{.assetStore = store, .renderer = &renderer};
  SceneManager manager;
  StandardScene* scene = nullptr;
  PrefabSystem* prefabs = nullptr;
  PhysicsSystem3D* physics = nullptr;

  explicit World(const Project& project)
      : database(project.folder,
                 AssetDatabase::Mode::ReadWrite,
                 AssetDatabase::projectFolders())
  {
    store.setDatabase(&database);
    manager.setSceneServices(&services);
    scene = manager.createScene<StandardScene>();
    prefabs = scene->tryGetSystem<PrefabSystem>();
    physics = &scene->addSystem<PhysicsSystem3D>();
  }
  ~World() { manager.destroyAll(); }

  void step() { scene->update(1.0 / 60.0); }

  std::size_t named(const std::string& name)
  {
    return scene->findEntities(name).size();
  }
};

TransformComponent at(glm::vec3 position, float yaw = 0.0f, float scale = 1.0f)
{
  TransformComponent t;
  t.position = position;
  t.rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f));
  t.scale = glm::vec3(scale);
  return t;
}
} // namespace

int main()
{
  scripts().add({.name = "Counter",
                 .kind = ScriptType::Kind::GameObject,
                 .createObject = []() -> std::unique_ptr<GameObject>
                 { return std::make_unique<Counter>(); }});
  const Project project;

  TEST("every StandardScene should have a PrefabSystem")
  {
    World world(project);
    CHECK(world.prefabs != nullptr);
  }

  TEST("spawn should place the prefab's root at the transform, with its "
       "children and scripts")
  {
    World world(project);
    Counter::created = 0;
    const Entity root =
        world.prefabs->spawn(project.tree, at({10, 20, 1}, 0.5f, 2.0f));
    CHECK(root.isValid());
    CHECK(Counter::created == 2);
    const TransformComponent& t = root.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, 10.0f) &&
          testing::approx(t.position.y, 20.0f));
    CHECK(testing::approx(t.scale.x, 2.0f));
    CHECK(root.hasComponent<CapsuleCollider3DComponent>());
    world.step();
    const Entity crown = world.scene->findEntity("Tree/Crown");
    CHECK(crown.isValid());
    // Its child keeps its place in the prefab, relative to the root (scaled).
    CHECK(testing::approx(hierarchy::worldPosition(crown).z, 11.0f, 1e-4f));
  }

  TEST("spawn should put the copy under the parent")
  {
    World world(project);
    Entity group = world.scene->createEntity();
    group.addComponent<TransformComponent>(
        TransformComponent{glm::vec3(100, 0, 0)});
    const Entity root =
        world.prefabs->spawn(project.post, at({1, 0, 0}), group);
    CHECK(hierarchy::parentOf(root) == group);
    CHECK(testing::approx(hierarchy::worldPosition(root).x, 101.0f));
    // A root without a transform in its prefab gets one.
    CHECK(root.hasComponent<TransformComponent>());
  }

  TEST("a spawned collider should become a static physics body where it was "
       "spawned")
  {
    World world(project);
    const Entity root = world.prefabs->spawn(project.tree, at({30, -5, 0}));
    const uint64_t before = world.physics->revision();
    world.step();
    CHECK(world.physics->hasBody(root));
    CHECK(world.physics->revision() != before);
    // A ray across at trunk height hits it 0.5 m short of its centre.
    const auto hit =
        world.physics->raycast(Ray{{20.0f, -5.0f, 1.0f}, {1.0f, 0.0f, 0.0f}});
    CHECK(hit.has_value() && hit->entity == root);
    CHECK(hit && testing::approx(hit->distance, 9.5f, 1e-2f));
    // ... and the static snapshot navigation plans against has it.
    CHECK(world.physics->snapshot()->bodies().size() == 1u);
  }

  TEST("spawning an unreadable prefab should log, return an invalid entity and "
       "make nothing")
  {
    World world(project);
    world.step();
    const std::size_t before = world.scene->entities().size();
    CHECK(
        !world.prefabs->spawn("asset:00000000000000000000000000000000", at({}))
             .isValid());
    CHECK(!world.prefabs->spawn("not a prefab", at({})).isValid());
    world.step();
    CHECK(world.scene->entities().size() == before);
  }

  TEST("spawning under a destroyed parent should return an invalid entity")
  {
    World world(project);
    Entity gone = world.scene->createEntity();
    world.scene->destroyEntity(gone);
    world.step();
    CHECK(!world.prefabs->spawn(project.post, at({}), gone).isValid());
  }

  TEST("despawn should remove the copy, its children and its body")
  {
    World world(project);
    const Entity root = world.prefabs->spawn(project.tree, at({0, 0, 0}));
    world.step();
    CHECK(world.physics->hasBody(root));
    world.prefabs->despawn(root);
    world.step();
    world.step();
    CHECK(!root.isAlive());
    CHECK(world.named("Tree") == 0u && world.named("Crown") == 0u);
    CHECK(!world.physics->hasBody(root));
    CHECK(world.scene->findObjects<Counter>().empty());
    world.prefabs->despawn(root); // dead: ignored
    world.prefabs->despawn(Entity());
  }

  TEST("1000 spawns (a forest at load) should take well under a second, "
       "physics included")
  {
    World world(project);
    Entity group = world.scene->createEntity();
    group.addComponent<TransformComponent>();
    const auto start = std::chrono::steady_clock::now();
    int made = 0;
    for (int i = 0; i < 1000; ++i)
      made += world.prefabs
                  ->spawn(project.tree,
                          at({float(i % 40) * 5.0f, float(i / 40) * 5.0f, 0}),
                          group)
                  .isValid();
    const auto spawned = std::chrono::steady_clock::now();
    world.step(); // their bodies made
    const auto stepped = std::chrono::steady_clock::now();
    const double spawnMs =
        std::chrono::duration<double, std::milli>(spawned - start).count();
    const double bodyMs =
        std::chrono::duration<double, std::milli>(stepped - spawned).count();
    std::printf(
        "  1000 spawns: %.1f ms, their bodies on the next update: %.1f ms\n",
        spawnMs,
        bodyMs);
    CHECK(made == 1000);
    CHECK(world.physics->snapshot()->bodies().size() == 1000u);
    CHECK(spawnMs + bodyMs < 1000.0);
  }

  return testing::report("prefabSystemTests");
}
