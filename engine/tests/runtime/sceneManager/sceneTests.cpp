#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/components/nameComponent.h>
#include <engine/core/components/parentComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/util/fatalError.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/game/gameObject.h>
#include <engine/runtime/rendering/standardScene.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/sceneManager/sceneManager.h>

#include <filesystem>
#include <fstream>

#include <string>
#include <utility>

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

// Counts its calls; does what its fields say as it updates.
class Actor : public GameObject
{
public:
  int* updates = nullptr;
  int* destroys = nullptr;
  int spawn = 0;                // objects to make on each update
  bool destroySelf = false;     // destroy itself on its next update
  GameObject* victim = nullptr; // destroy this on its next update

protected:
  void onUpdate(double) override
  {
    if (updates)
      ++*updates;
    for (int i = 0; i < spawn; ++i)
      scene().createObject<Actor>().updates = updates;
    if (victim)
      scene().destroyObject(std::exchange(victim, nullptr));
    if (destroySelf)
      scene().destroyObject(this);
  }
  void onDestroy() override
  {
    if (destroys)
      ++*destroys;
  }
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

  TEST("view should list the entities with all the given components")
  {
    World world;
    Entity named = world.named("Farm");
    Entity tagged = world.named("Gate");
    tagged.addComponent<ParentComponent>();
    world.scene.flushEntities();

    CHECK(world.scene.view<NameComponent>().size() == 2u);
    const std::vector<Entity> both =
        world.scene.view<NameComponent, ParentComponent>();
    CHECK(both.size() == 1u && both[0] == tagged);
    CHECK(named.isValid());
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
    CHECK(
        !world.scene.findEntity("North/Gate").isValid()); // North isn't a root
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

  TEST("StandardScene::play should replace the contents with a scene file")
  {
    AssetStore store;
    testing3d::MockRenderer renderer;
    SceneServices services{.assetStore = store, .renderer = &renderer};
    SceneManager manager;
    manager.setSceneServices(&services);
    StandardScene& scene = *manager.createScene<StandardScene>();
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

  TEST("objects made during an update should be kept, and start next frame")
  {
    World world;
    int updates = 0;
    Actor& spawner = world.scene.createObject<Actor>();
    spawner.updates = &updates;
    spawner.spawn = 50; // grows the list well past its capacity mid-loop
    world.scene.update(0.016);
    CHECK(updates == 1); // only the spawner ran this frame
    CHECK(world.scene.findObjects<Actor>().size() == 51u);
    spawner.spawn = 0;
    world.scene.update(0.016);
    CHECK(updates == 1 + 51);
  }

  TEST("an object destroyed during an update should be deleted after it, its "
       "onDestroy run once")
  {
    World world;
    int updates = 0, destroys = 0;
    Actor& killer = world.scene.createObject<Actor>();
    Actor& victim = world.scene.attachObject<Actor>(world.named("Victim"));
    victim.updates = &updates;
    victim.destroys = &destroys;
    killer.victim = &victim;
    Actor& suicidal = world.scene.createObject<Actor>();
    suicidal.destroys = &destroys;
    suicidal.destroySelf = true;

    world.scene.update(0.016); // the victim comes after its killer
    CHECK(updates == 0);       // destroyed before its turn: not called
    CHECK(destroys == 2);
    CHECK(world.scene.findObjects<Actor>().size() == 1u);
    CHECK(world.scene.findEntity("Victim").isValid()); // gone at next flush
    world.scene.flushEntities();
    CHECK(!world.scene.findEntity("Victim").isValid());
  }

  TEST("destroying an object without an entity, or twice, should be safe")
  {
    World world;
    int destroys = 0;
    Actor& loose = world.scene.createObject<Actor>();
    loose.destroys = &destroys;
    world.scene.destroyObject(&loose); // outside an update: deleted at once
    CHECK(destroys == 1);
    CHECK(world.scene.findObjects<Actor>().empty());
    world.scene.destroyObject(nullptr);
  }

  TEST("destroying an entity should destroy its game object")
  {
    World world;
    int updates = 0, destroys = 0;
    Entity entity = world.named("Thing");
    Actor& actor = world.scene.attachObject<Actor>(entity);
    actor.updates = &updates;
    actor.destroys = &destroys;
    world.scene.flushEntities();

    world.scene.destroyEntity(entity);
    world.scene.update(0.016); // the flush at its start takes the entity
    CHECK(destroys == 1);
    CHECK(updates == 0);
    CHECK(world.scene.findObjects<Actor>().empty());
  }

  TEST("a scene's objects should get onDestroy as the scene manager ends it")
  {
    AssetStore store;
    SceneServices services{.assetStore = store};
    SceneManager manager;
    manager.setSceneServices(&services);
    Scene& scene = *manager.createScene<Scene>();
    int destroys = 0;
    scene.createObject<Actor>().destroys = &destroys;
    scene.createObject<Actor>().destroys = &destroys;
    manager.destroyAll();
    CHECK(destroys == 2);
  }

  TEST("renderer() and game() without them should be fatal errors, not aborts")
  {
    World world;
    bool renderer = false, game = false;
    try
    {
      (void)world.scene.renderer();
    }
    catch (const FatalError&)
    {
      renderer = true;
    }
    try
    {
      (void)world.scene.game();
    }
    catch (const FatalError&)
    {
      game = true;
    }
    CHECK(renderer);
    CHECK(game);
  }

  return testing::report("sceneTests");
}
