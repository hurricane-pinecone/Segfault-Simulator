#include "../../testHarness.h"
#include "../rendering3d/mockRenderer3D.h"

#include <engine/core/components/nameComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/game/gameObject.h>
#include <engine/runtime/rendering3d/scene3D.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/sceneManager/sceneManager.h>

#include <filesystem>
#include <fstream>

#include <string>

using namespace sfs;

namespace
{
struct World
{
  AssetStore store;
  Scene scene{0, SceneServices{.assetStore = store}};

  Entity named(const std::string& name, const Entity& parent = {})
  {
    Entity entity = scene.createEntity();
    entity.addComponent<NameComponent>(name);
    if (parent.isValid())
      hierarchy::setParent(entity, parent);
    return entity;
  }
};

class Probe : public GameObject
{
public:
  Scene* seen = nullptr;

protected:
  void onCreate(Scene&) override { seen = &scene(); }
};
} // namespace

int main()
{
  TEST("findEntity should find by name, parents before children")
  {
    World world;
    Entity farm = world.named("Farm");
    Entity gate = world.named("Gate", farm);
    Entity loose = world.named("Gate"); // a second Gate, a root made later
    world.scene.flushEntities();

    CHECK(world.scene.findEntity("Farm") == farm);
    CHECK(world.scene.findEntity("Gate") == gate); // Farm's subtree comes first
    CHECK(!world.scene.findEntity("Barn").isValid());
    CHECK(world.scene.findEntities("Gate").size() == 2u);
    CHECK(world.scene.findEntities("Gate")[1] == loose);
  }

  TEST("findEntity should follow a path of names from a root")
  {
    World world;
    Entity fences = world.named("Fences");
    Entity north = world.named("North", fences);
    Entity gate = world.named("Gate", north);
    Entity strayGate = world.named("Gate");
    world.scene.flushEntities();

    CHECK(world.scene.findEntity("Fences/North/Gate") == gate);
    CHECK(world.scene.findEntity("Fences/North") == north);
    CHECK(!world.scene.findEntity("North/Gate").isValid()); // North isn't a root
    CHECK(!world.scene.findEntity("Fences/South/Gate").isValid());
    (void)strayGate;
  }

  TEST("destroyBranch should destroy an entity and everything below it")
  {
    World world;
    Entity cart = world.named("Cart");
    Entity crate = world.named("Crate", cart);
    Entity lid = world.named("Lid", crate);
    Entity ball = world.named("Ball");
    world.scene.flushEntities();

    world.scene.destroyBranch(cart);
    world.scene.flushEntities();
    CHECK(!cart.isAlive());
    CHECK(!crate.isAlive());
    CHECK(!lid.isAlive());
    CHECK(ball.isAlive());
  }

  TEST("a game object should know its scene by onCreate")
  {
    World world;
    Entity entity = world.named("Thing");
    Probe& probe = world.scene.attachObject<Probe>(entity);
    CHECK(probe.seen == &world.scene);
    CHECK(&probe.scene() == &world.scene);
    CHECK(world.scene.tryGame() == nullptr); // no game outside one
  }

  TEST("Scene3D::play should replace the contents with a scene file")
  {
    AssetStore store;
    testing3d::MockRenderer3D renderer;
    SceneServices services{.assetStore = store, .renderer3D = &renderer};
    SceneManager manager;
    manager.setSceneServices(&services);
    Scene3D& scene = *manager.createScene<Scene3D>();
    scene.createEntity().addComponent<NameComponent>("Old");
    scene.flushEntities();

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "sfs_scene3d_play.scene";
    std::ofstream(file) << R"({"formatVersion": 2, "systems": [],
      "entities": [{"NameComponent": {"name": "New"},
                    "children": [{"NameComponent": {"name": "Child"}}]}]})";

    CHECK(scene.play(file));
    CHECK(!scene.findEntity("Old").isValid());
    CHECK(scene.findEntity("New/Child").isValid());
    CHECK(scene.entities().size() == 2u);
    CHECK(!scene.play(file.string() + ".missing"));
    std::filesystem::remove(file);
  }

  return testing::report("sceneTests");
}
