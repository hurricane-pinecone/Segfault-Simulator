#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/core/components/animatorComponent.h>
#include <engine/core/components/inputContextComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/parentComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/particles/particleEffect.h>
#include <engine/core/util/fatalError.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/game/gameObject.h>
#include <engine/runtime/input/input.h>
#include <engine/runtime/rendering/modules/particleModule.h>
#include <engine/runtime/rendering/renderSystem.h>
#include <engine/runtime/rendering/standardScene.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/sceneManager/sceneManager.h>

#include <filesystem>
#include <fstream>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
// Keeps the game time it's given and the registry's game time as it reads
// it, each update.
class Timed : public System
{
public:
  explicit Timed(TimeStep kind) { setTimeStep(kind); }

  Registry& world() { return *registry; }

  double given = 0.0;
  std::vector<double> seen;
  // Whether the registry's game time was the sum of the steps given, each
  // time it was read.
  bool agreed = true;

protected:
  void update(double deltaTime) override
  {
    given += deltaTime;
    seen.push_back(registry->gameTime());
    agreed = agreed && std::abs(registry->gameTime() - given) < 1e-9;
  }
};

// Notes whether it saw Space and Up pressed, and its turn among the calls.
class Listener : public GameObject
{
public:
  bool space = false;
  bool up = false;
  std::vector<std::string>* order = nullptr;
  std::string name;

  // Gives it an input context claiming `keys`.
  void claim(std::vector<std::string> keys, int priority, bool active = true)
  {
    entity().addComponent<InputContextComponent>(
        InputContextComponent{std::move(keys), priority, active});
  }

protected:
  void onProcessInput(const Input& input) override
  {
    space = input.keyboard().keyPressed(Key::Space);
    up = input.keyboard().keyPressed(Key::Up);
  }
  void onUpdate(double) override
  {
    if (order)
      order->push_back(name);
  }
};

// Notes its update and late update in a shared list.
class Late : public System
{
public:
  explicit Late(std::vector<std::string>& order, std::string name)
      : m_order(order), m_name(std::move(name))
  {
  }
  double lateGiven = 0.0;

protected:
  void update(double) override { m_order.push_back(m_name + ".update"); }
  void lateUpdate(double deltaTime) override
  {
    lateGiven += deltaTime;
    m_order.push_back(m_name + ".late");
  }

private:
  std::vector<std::string>& m_order;
  std::string m_name;
};

// Space and Up pressed this frame.
Input spaceAndUp()
{
  Input input;
  input.keyboard().beginFrame();
  input.keyboard().pressKey(SDL_SCANCODE_SPACE);
  input.keyboard().pressKey(SDL_SCANCODE_UP);
  return input;
}

std::string base64(const std::vector<uint8_t>& bytes)
{
  static const char* digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3)
  {
    const uint32_t n = (bytes[i] << 16) |
                       (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) |
                       (i + 2 < bytes.size() ? bytes[i + 2] : 0);
    out += digits[(n >> 18) & 63];
    out += digits[(n >> 12) & 63];
    out += i + 1 < bytes.size() ? digits[(n >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? digits[n & 63] : '=';
  }
  return out;
}

template <typename T>
void append(std::vector<uint8_t>& out, const std::vector<T>& values)
{
  const auto* bytes = reinterpret_cast<const uint8_t*>(values.data());
  out.insert(out.end(), bytes, bytes + values.size() * sizeof(T));
}

// A .gltf of one triangle, "Box", with a one-second clip, "Move", sliding it
// one along x.
void writeMovingModel(const std::filesystem::path& file)
{
  std::vector<uint8_t> buffer;
  append(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0}); // 36 bytes
  append(buffer, std::vector<uint16_t>{0, 1, 2, 0});             // 8 bytes
  append(buffer, std::vector<float>{0, 1});                      // 8 bytes
  append(buffer, std::vector<float>{0, 0, 0, 1, 0, 0});          // 24 bytes
  std::ofstream(file) << R"({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "Box", "mesh": 0}],
    "meshes": [{"name": "Box", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "animations": [{"name": "Move",
      "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}],
      "samplers": [{"input": 2, "output": 3}]}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
      {"bufferView": 2, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
      {"bufferView": 3, "componentType": 5126, "count": 2, "type": "VEC3"}
    ],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6},
      {"buffer": 0, "byteOffset": 44, "byteLength": 8},
      {"buffer": 0, "byteOffset": 52, "byteLength": 24}
    ],
    "buffers": [{"byteLength": 76, "uri": "data:application/octet-stream;base64,)" +
                             base64(buffer) + R"("}]
  })";
}

// Every live particle's place, in order.
std::vector<glm::vec2> particlePlaces(const ParticleEngine& particles)
{
  std::vector<ParticleSpriteBatch> batches;
  particles.sprites(batches);
  std::vector<glm::vec2> places;
  for (const ParticleSpriteBatch& batch : batches)
    for (const ParticleSprite& sprite : batch.sprites)
      places.push_back(sprite.position);
  return places;
}
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

  TEST("query should list the flushed entities with all the given components")
  {
    World world;
    Entity named = world.named("Farm");
    Entity tagged = world.named("Gate");
    tagged.addComponent<ParentComponent>();
    CHECK(world.scene.query<NameComponent>().empty()); // (until the flush)
    world.scene.flushEntities();

    CHECK(world.scene.query<NameComponent>().size() == 2u);
    const std::vector<Entity>& both =
        world.scene.query<NameComponent, ParentComponent>();
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

  TEST("gameTime should be the sum of the game time given, read on each step "
       "and at the frame's end, at any time scale")
  {
    for (const double scale : {1.0, 4.0, 16.0})
    {
      World world;
      Timed& stepped = world.scene.addSystem<Timed>(System::TimeStep::Stepped);
      Registry& registry = stepped.world();
      Timed& frame = world.scene.addSystem<Timed>(System::TimeStep::Frame);
      registry.setTimeScale(scale);
      registry.setMaxTimeStep(1.0 / 30.0);
      CHECK(registry.gameTime() == 0.0);
      for (int i = 0; i < 10; ++i)
        world.scene.update(1.0 / 60.0);
      CHECK(stepped.agreed);
      CHECK(frame.agreed);
      CHECK(frame.seen.size() == 10u);
      CHECK(stepped.seen.size() > frame.seen.size() || scale == 1.0);
      CHECK(testing::approx(registry.gameTime(), 10.0 / 60.0 * scale));
      CHECK(testing::approx(frame.seen.back(), registry.gameTime()));
    }
  }

  TEST("while paused, systems should still get their game time and gameTime "
       "advance, while animators and particles stand still")
  {
    const std::filesystem::path project =
        std::filesystem::temp_directory_path() / "sfsSceneTests" / "project";
    std::filesystem::create_directories(project / "assets");
    writeMovingModel(project / "assets" / "box.gltf");
    std::ofstream(project / "assets" / "box.gltf.meta")
        << R"({"formatVersion": 1, "id": "0123456789abcdef0123456789abcd01"})";
    const AssetDatabase database(project,
                                 AssetDatabase::Mode::ReadOnly,
                                 AssetDatabase::projectFolders());
    const AssetRecord* record = database.findByPath("assets/box.gltf");
    CHECK(record != nullptr);

    AssetStore store;
    store.setDatabase(&database);
    testing3d::MockRenderer renderer;
    SceneServices services{.assetStore = store, .renderer = &renderer};
    SceneManager manager;
    manager.setSceneServices(&services);
    StandardScene& scene = *manager.createScene<StandardScene>();
    Timed& stepped = scene.addSystem<Timed>(System::TimeStep::Stepped);
    Registry& registry = stepped.world();

    Entity rig = scene.createEntity();
    rig.addComponent<NameComponent>("Rig");
    rig.addComponent<TransformComponent>();
    rig.addComponent<AnimatorComponent>();
    AnimatorComponent& animator = rig.getComponent<AnimatorComponent>();
    animator.clip = "Move";
    if (record)
      animator.source = "asset:" + record->id.toString();
    Entity box = scene.createEntity();
    box.addComponent<NameComponent>("Box");
    box.addComponent<TransformComponent>();
    hierarchy::setParent(box, rig);

    ParticleModule* particles = scene.renderSystem().module<ParticleModule>();
    CHECK(particles != nullptr);
    if (!particles)
      TEST("an input context should take its keys from lower contexts and from "
           "objects with none, but not from its own object")
      {
        World world;
        Listener& driver =
            world.scene.attachObject<Listener>(world.scene.createEntity());
        Listener& camera =
            world.scene.attachObject<Listener>(world.scene.createEntity());
        Listener& planner =
            world.scene.attachObject<Listener>(world.scene.createEntity());
        driver.claim({"Space", "Up"}, 2);
        planner.claim({"Up"}, 1);
        world.scene.flushEntities();
        world.scene.processInput(spaceAndUp());
        CHECK(driver.space && driver.up);
        CHECK(!camera.space && !camera.up);
        CHECK(!planner.space && !planner.up);
      }

    TEST("an inactive input context should withhold nothing, and equal "
         "priorities should share")
    {
      World world;
      Listener& driver =
          world.scene.attachObject<Listener>(world.scene.createEntity());
      Listener& camera =
          world.scene.attachObject<Listener>(world.scene.createEntity());
      Listener& planner =
          world.scene.attachObject<Listener>(world.scene.createEntity());
      driver.claim({"Space"}, 2, /*active*/ false);
      planner.claim({"Up", "Nonsense"}, 1);
      camera.claim({"Up"}, 1);
      world.scene.flushEntities();
      world.scene.processInput(spaceAndUp());
      CHECK(camera.space && planner.space); // the driver's is off
      CHECK(camera.up && planner.up);       // shared at priority 1
      CHECK(driver.space && !driver.up);    // its own is off: it ranks lowest
      CHECK(
          planner.entity().getComponent<InputContextComponent>().loggedUnknown);
    }

    TEST("lateUpdate should run on enabled systems in order, after every "
         "object's update")
    {
      World world;
      std::vector<std::string> order;
      Late& first = world.scene.addSystem<Late>(order, "first");
      Late& second = world.scene.addSystem<Late>(order, "second");
      Late& off = world.scene.addSystem<Late>(order, "off");
      off.setEnabled(false);
      Listener& object = world.scene.createObject<Listener>();
      object.order = &order;
      object.name = "object";
      world.scene.update(0.5);
      const std::vector<std::string> expected = {"first.update",
                                                 "second.update",
                                                 "object",
                                                 "first.late",
                                                 "second.late"};
      CHECK(order == expected);
      CHECK(testing::approx(first.lateGiven, 0.5));
      CHECK(testing::approx(second.lateGiven, 0.5));
    }

    return testing::report("sceneTests");
    ParticleEffectDesc spark;
    spark.burstCount = 3;
    spark.lifetime = {10.0f, 10.0f};
    spark.speed = {1.0f, 1.0f};
    particles->registerEffect("spark", spark);
    particles->spawnBurst("spark", {0.0f, 0.0f}, 0.0f);

    // Running: the clip plays and the particles fly.
    scene.update(0.1);
    const float playedTo = rig.getComponent<AnimatorComponent>().time;
    const std::vector<glm::vec2> flown = particlePlaces(*particles);
    CHECK(playedTo > 0.0f);
    CHECK(flown.size() == 3u);

    registry.setPaused(true);
    CHECK(registry.paused());
    const double before = registry.gameTime();
    scene.update(0.25);
    CHECK(testing::approx(stepped.given, 0.35));
    CHECK(testing::approx(registry.gameTime(), before + 0.25));
    CHECK(rig.getComponent<AnimatorComponent>().time == playedTo);
    CHECK(particlePlaces(*particles) == flown);

    registry.setPaused(false);
    scene.update(0.1);
    CHECK(rig.getComponent<AnimatorComponent>().time > playedTo);
    CHECK(particlePlaces(*particles) != flown);
  }

  return testing::report("sceneTests");
}
