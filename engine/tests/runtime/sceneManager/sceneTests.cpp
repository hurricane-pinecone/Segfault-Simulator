#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/core/components/animatorComponent.h>
#include <engine/core/components/inputContextComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/particles/particleEffect.h>
#include <engine/core/reflection/reflection.h>
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
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace sfs;

namespace
{
struct Tagged
{
};

struct [[sfs::scene_data]] Scoreboard
{
  int home = 0;
};

struct World
{
  AssetStore store;
  Scene scene{0, SceneBackends{.assetStore = store}};

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
  explicit Timed(TimeStep kind) : m_kind(kind) {}

  Registry& world() { return *registry; }

  double given = 0.0;
  std::vector<double> seen;
  // Whether the registry's game time was the sum of the steps given, each
  // time it was read.
  bool agreed = true;

protected:
  void create() override { setTimeStep(m_kind); }
  void update(double deltaTime) override
  {
    given += deltaTime;
    seen.push_back(registry->gameTime());
    agreed = agreed && std::abs(registry->gameTime() - given) < 1e-9;
  }

private:
  TimeStep m_kind;
};

struct [[sfs::event_news]] BeatEvent
{
  int number = 0;
};

// Sends a BeatEvent, numbered from 1, on each update.
class BeatWriter : public System
{
public:
  explicit BeatWriter(TimeStep kind) : m_kind(kind) {}

  Registry& world() { return *registry; }

  int sent = 0;

protected:
  void create() override { setTimeStep(m_kind); }
  void update(double) override { registry->events().send(BeatEvent{++sent}); }

private:
  TimeStep m_kind;
};

// Reads the Beats on each update, noting each update's.
class BeatReader : public System
{
public:
  explicit BeatReader(TimeStep kind) : m_kind(kind) {}

  std::vector<std::vector<int>> seen;

protected:
  void create() override
  {
    setTimeStep(m_kind);
    m_beats = consumer<BeatEvent>();
  }
  void update(double) override
  {
    std::vector<int>& now = seen.emplace_back();
    for (const BeatEvent& beat : m_beats.read())
      now.push_back(beat.number);
  }

private:
  TimeStep m_kind;
  EventConsumer<BeatEvent> m_beats;
};

// A turn number on an entity, replaced by TurnService and read by
// TurnSystem.
struct Turn
{
  int number = 0;
};

struct [[sfs::event_entity_news]] TurnEvent
{
  int number = 0;
};

// Gives an entity the turn its news names, one add over the old turn.
class TurnService : public Service
{
protected:
  void create() override
  {
    m_turns = consumer<TurnEvent>().onLatest(
        [](const Entity& entity, const TurnEvent& turn)
        {
          if (entity)
            entity.addComponent<Turn>(Turn{turn.number});
        });
  }

private:
  EventConsumer<TurnEvent> m_turns;
};

// Each step notes its entity's turn and asks for the next.
class TurnSystem : public System
{
public:
  Entity turning;
  std::vector<int> seen;

protected:
  void update(double) override
  {
    const Turn* turn = turning.getComponent<Turn>();
    const int number = turn ? turn->number : 0;
    seen.push_back(number);
    send(turning, TurnEvent{number + 1});
  }
};

// A request and its response, for the responses taken in between steps.
struct [[sfs::event_response]] ToldResponse
{
  int value = 0;
};
struct [[sfs::event_request(ToldResponse)]] AskRequest
{
};

// Asks once for its entity, then notes the responses each step.
class Asker : public System
{
public:
  Entity asking;
  std::vector<std::vector<int>> seen;

protected:
  void create() override
  {
    m_asks = requester<AskRequest>().onResponse(
        [this](const Entity&, const ToldResponse& told)
        { m_told.push_back(told.value); });
  }
  void update(double) override
  {
    seen.push_back(std::move(m_told));
    m_told.clear();
    if (seen.size() == 1)
      m_asks.request(asking, AskRequest{});
  }

private:
  EventRequester<AskRequest> m_asks;
  // The responses its handler got before this update.
  std::vector<int> m_told;
};

// Consumes each request and responds from a worker thread.
class Answerer : public System
{
protected:
  void create() override { m_asks = consumer<AskRequest>(); }
  void update(double) override
  {
    for (auto [entity, ask, responder] : m_asks.read())
    {
      std::thread worker([held = responder] { held.respond(ToldResponse{5}); });
      worker.join();
    }
  }

private:
  EventConsumer<AskRequest> m_asks;
};

// Sends a BeatEvent when it's destroyed, numbered by `number`.
class BeatOnDestroy : public GameObject
{
public:
  int number = 0;

protected:
  void onDestroy() override { scene().events().send(BeatEvent{number}); }
};

// Destroys itself from a beat handler; counts its onUpdates where told (it's
// deleted once the frame's updates are done).
class BeatQuitter : public GameObject
{
public:
  int* updates = nullptr;

protected:
  void onCreate(Scene& scene) override
  {
    m_beats = scene.events().consumer<BeatEvent>().onEvent(
        [this](const BeatEvent&) { this->scene().destroyObject(this); });
  }
  void onUpdate(double) override
  {
    if (updates)
      ++*updates;
  }

private:
  EventConsumer<BeatEvent> m_beats;
};

// Counts beats with a handler (before its onUpdate) and reads them again in
// onUpdate: the same span.
class BeatCounter : public GameObject
{
public:
  std::vector<int> handled;
  std::vector<std::size_t> readInUpdate;

protected:
  void onCreate(Scene& scene) override
  {
    m_beats = scene.events().consumer<BeatEvent>().onEvent(
        [this](const BeatEvent& beat) { handled.push_back(beat.number); });
  }
  void onUpdate(double) override
  {
    readInUpdate.push_back(m_beats.read().size());
  }

private:
  EventConsumer<BeatEvent> m_beats;
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
  TEST("a scene's scene data should be added, read const and removed")
  {
    World world;
    CHECK(world.scene.sceneData<const Scoreboard>() == nullptr);
    Scoreboard& board = world.scene.addSceneData<Scoreboard>(Scoreboard{2});
    const Scoreboard* read = world.scene.sceneData<const Scoreboard>();
    static_assert(std::is_same_v<decltype(read), const Scoreboard*>);
    CHECK(read == &board && read->home == 2);
    CHECK(world.scene.sceneData<Scoreboard>() == &board);
    world.scene.removeSceneData<Scoreboard>();
    CHECK(world.scene.sceneData<const Scoreboard>() == nullptr);
  }

  TEST("findEntity should find by name, parents before children")
  {
    World world;
    Entity town = world.named("Town");
    Entity gate = world.named("Gate", town);
    Entity loose = world.named("Gate"); // a second Gate, a root made later
    world.scene.flushEntities();

    CHECK(world.scene.findEntity("Town") == town);
    CHECK(world.scene.findEntity("Gate") == gate); // Town's subtree comes first
    CHECK(!world.scene.findEntity("House").isValid());
    CHECK(world.scene.findEntities("Gate").size() == 2u);
    CHECK(world.scene.findEntities("Gate")[1] == loose);
  }

  TEST("query should list the flushed entities with all the given components")
  {
    World world;
    Entity named = world.named("Town");
    Entity tagged = world.named("Gate");
    tagged.addComponent<Tagged>();
    CHECK(world.scene.query<NameComponent>().empty()); // (until the flush)
    world.scene.flushEntities();

    CHECK(world.scene.query<NameComponent>().size() == 2u);
    const std::vector<Entity>& both =
        world.scene.query<NameComponent, Tagged>();
    CHECK(both.size() == 1u && both[0] == tagged);
    CHECK(named.isValid());
  }

  TEST("holdersOf should list the holders of a reflected component type")
  {
    World world;
    Entity town = world.named("Town");
    Entity gate = world.named("Gate");
    world.scene.flushEntities();

    const reflect::TypeInfo* name = reflect::types().find("NameComponent");
    CHECK(name != nullptr);
    if (name)
    {
      const std::vector<Entity>& holders = world.scene.holdersOf(*name);
      CHECK(holders.size() == 2u && holders[0] == town && holders[1] == gate);
    }
    const reflect::TypeInfo notComponent{
        .name = "Settings", .kind = reflect::TypeKind::ScriptableObject};
    CHECK(world.scene.holdersOf(notComponent).empty());
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
    CHECK(world.scene.findEntity("Gate") == gate); // the first in preorder
    // A reparent counts at once, with no flush.
    hierarchy::setParent(strayGate, fences);
    CHECK(world.scene.findEntity("Fences/Gate") == strayGate);
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
    CHECK(!cart.isValid());
    CHECK(!crate.isValid());
    CHECK(!lid.isValid());
    CHECK(ball.isValid());
    CHECK(world.scene.entities().size() == 1u);
    world.scene.destroyBranch(cart); // already gone: nothing happens
    world.scene.flushEntities();
    CHECK(ball.isValid());
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
    SceneBackends backends{.assetStore = store, .renderer = &renderer};
    SceneManager manager;
    manager.setSceneBackends(&backends);
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
    SceneBackends backends{.assetStore = store};
    SceneManager manager;
    manager.setSceneBackends(&backends);
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
    SceneBackends backends{.assetStore = store, .renderer = &renderer};
    SceneManager manager;
    manager.setSceneBackends(&backends);
    StandardScene& scene = *manager.createScene<StandardScene>();
    Timed& stepped = scene.addSystem<Timed>(System::TimeStep::Stepped);
    Registry& registry = stepped.world();

    Entity rig = scene.createEntity();
    rig.addComponent<NameComponent>("Rig");
    rig.addComponent<TransformComponent>();
    rig.addComponent<AnimatorComponent>();
    AnimatorComponent* animator = rig.getComponent<AnimatorComponent>();
    animator->clip = "Move";
    if (record)
      animator->source = "asset:" + record->id.toString();
    Entity box = scene.createEntity();
    box.addComponent<NameComponent>("Box");
    box.addComponent<TransformComponent>();
    hierarchy::setParent(box, rig);

    ParticleModule* particles = scene.renderSystem().module<ParticleModule>();
    CHECK(particles != nullptr);
    if (!particles)
      continue;
    ParticleEffectDesc spark;
    spark.burstCount = 3;
    spark.lifetime = {10.0f, 10.0f};
    spark.speed = {1.0f, 1.0f};
    particles->registerEffect("spark", spark);
    particles->spawnBurst("spark", {0.0f, 0.0f}, 0.0f);

    // Running: the clip plays and the particles fly.
    scene.update(0.1);
    const float playedTo = rig.getComponent<AnimatorComponent>()->time;
    const std::vector<glm::vec2> flown = particlePlaces(*particles);
    CHECK(playedTo > 0.0f);
    CHECK(flown.size() == 3u);

    registry.setPaused(true);
    CHECK(registry.paused());
    const double before = registry.gameTime();
    scene.update(0.25);
    CHECK(testing::approx(stepped.given, 0.35));
    CHECK(testing::approx(registry.gameTime(), before + 0.25));
    CHECK(rig.getComponent<AnimatorComponent>()->time == playedTo);
    CHECK(particlePlaces(*particles) == flown);

    registry.setPaused(false);
    scene.update(0.1);
    CHECK(rig.getComponent<AnimatorComponent>()->time > playedTo);
    CHECK(particlePlaces(*particles) != flown);
  }

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
        planner.entity().getComponent<InputContextComponent>()->loggedUnknown);
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
    const std::vector<std::string> expected = {
        "first.update", "second.update", "object", "first.late", "second.late"};
    CHECK(order == expected);
    CHECK(testing::approx(first.lateGiven, 0.5));
    CHECK(testing::approx(second.lateGiven, 0.5));
  }

  TEST("events from 3 steps a frame should reach a Frame reader once each, "
       "and a Stepped reader before the writer a step later")
  {
    World world;
    BeatReader& early =
        world.scene.addSystem<BeatReader>(System::TimeStep::Stepped);
    BeatWriter& writer =
        world.scene.addSystem<BeatWriter>(System::TimeStep::Stepped);
    BeatReader& frame =
        world.scene.addSystem<BeatReader>(System::TimeStep::Frame);
    Registry& registry = writer.world();
    registry.setMaxTimeStep(1.0 / 180.0);
    world.scene.update(1.0 / 60.0);
    world.scene.update(1.0 / 60.0);
    CHECK(writer.sent == 6);
    CHECK(frame.seen.size() == 2u);
    if (frame.seen.size() == 2u)
    {
      CHECK((frame.seen[0] == std::vector<int>{1, 2, 3}));
      CHECK((frame.seen[1] == std::vector<int>{4, 5, 6}));
    }
    CHECK(early.seen.size() == 6u);
    if (early.seen.size() == 6u)
    {
      CHECK(early.seen[0].empty());
      for (int step = 1; step < 6; ++step)
        CHECK((early.seen[step] == std::vector<int>{step}));
    }
  }

  TEST("a worker's response should be read at the next step, not the next "
       "frame")
  {
    World world;
    Asker& asker = world.scene.addSystem<Asker>();
    world.scene.addSystem<Answerer>();
    asker.asking = world.scene.createEntity();
    BeatWriter& writer =
        world.scene.addSystem<BeatWriter>(System::TimeStep::Frame);
    writer.world().setMaxTimeStep(1.0 / 180.0);
    world.scene.update(1.0 / 60.0);
    CHECK(asker.seen.size() == 3u);
    if (asker.seen.size() == 3u)
    {
      CHECK(asker.seen[0].empty());
      CHECK((asker.seen[1] == std::vector<int>{5}));
      CHECK(asker.seen[2].empty());
    }
  }

  TEST("the frame's events should start after the flush")
  {
    World world;
    BeatReader& reader =
        world.scene.addSystem<BeatReader>(System::TimeStep::Frame);
    Entity doomed = world.scene.createEntity();
    world.scene.attachObject<BeatOnDestroy>(doomed).number = 7;
    world.scene.update(0.1);
    world.scene.destroyEntity(doomed);
    // The flush that starts this frame destroys the object, which sends.
    world.scene.update(0.1);
    CHECK(reader.seen.size() == 2u);
    if (reader.seen.size() == 2u)
      CHECK((reader.seen[1] == std::vector<int>{7}));
    // Its one consumer read it, so the next boundary drops it, and a new
    // consumer finds nothing.
    world.scene.update(0.1);
    CHECK(world.scene.events().consumer<BeatEvent>().unread() == 0u);
  }

  TEST("an object that destroys itself from a handler should get no "
       "onUpdate")
  {
    World world;
    world.scene.addSystem<BeatWriter>(System::TimeStep::Frame);
    int updates = 0;
    world.scene.attachObject<BeatQuitter>(world.scene.createEntity()).updates =
        &updates;
    world.scene.update(0.1);
    CHECK(updates == 0);
  }

  TEST("an object's consumer made in onCreate should run its handlers before "
       "onUpdate, which reads the same span")
  {
    World world;
    BeatWriter& writer =
        world.scene.addSystem<BeatWriter>(System::TimeStep::Frame);
    BeatCounter& counter =
        world.scene.attachObject<BeatCounter>(world.scene.createEntity());
    world.scene.update(0.1);
    world.scene.update(0.1);
    world.scene.update(0.1);
    CHECK(writer.sent == 3);
    // Systems update before objects: each frame's beat is new to it.
    CHECK((counter.handled == std::vector<int>{1, 2, 3}));
    CHECK((counter.readInUpdate == std::vector<std::size_t>{1, 1, 1}));
  }

  TEST("events should flow while paused, at time scale 0")
  {
    World world;
    BeatWriter& writer =
        world.scene.addSystem<BeatWriter>(System::TimeStep::Frame);
    BeatReader& reader =
        world.scene.addSystem<BeatReader>(System::TimeStep::Stepped);
    Registry& registry = writer.world();
    registry.setPaused(true);
    registry.setTimeScale(0.0);
    world.scene.update(0.1);
    world.scene.update(0.1);
    CHECK(writer.sent == 2);
    CHECK(reader.seen.size() == 2u);
    if (reader.seen.size() == 2u)
    {
      CHECK((reader.seen[0] == std::vector<int>{1}));
      CHECK((reader.seen[1] == std::vector<int>{2}));
    }
  }

  TEST("services should run at the start of every step, before the systems")
  {
    World world;
    world.scene.addService<TurnService>();
    TurnSystem& turns = world.scene.addSystem<TurnSystem>();
    turns.turning = world.scene.createEntity();
    BeatWriter& writer =
        world.scene.addSystem<BeatWriter>(System::TimeStep::Frame);
    writer.world().setMaxTimeStep(1.0 / 240.0);
    world.scene.update(1.0 / 60.0);
    world.scene.update(1.0 / 60.0);
    // Four steps a frame: the service's swap reaches the system every step.
    CHECK((turns.seen == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
    int services = 0;
    world.scene.forEachService([&](Service&) { ++services; });
    CHECK(services == 1);
  }

  TEST("services should still run while paused, at time scale 0")
  {
    World world;
    world.scene.addService<TurnService>();
    TurnSystem& turns = world.scene.addSystem<TurnSystem>();
    turns.turning = world.scene.createEntity();
    BeatWriter& writer =
        world.scene.addSystem<BeatWriter>(System::TimeStep::Frame);
    writer.world().setPaused(true);
    writer.world().setTimeScale(0.0);
    world.scene.update(0.1);
    world.scene.update(0.1);
    world.scene.update(0.1);
    CHECK((turns.seen == std::vector<int>{0, 1, 2}));
  }

  return testing::report("sceneTests");
}
