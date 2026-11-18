#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"
#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/core/events/eventConsumer.h"
#include "engine/core/logger/logger.h"
#include "engine/core/rendering/camera.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/facilities/animation.h"
#include "engine/runtime/facilities/assets.h"
#include "engine/runtime/facilities/cameras.h"
#include "engine/runtime/facilities/debugDraw.h"
#include "engine/runtime/facilities/graphics.h"
#include "engine/runtime/facilities/meshes.h"
#include "engine/runtime/facilities/navigation.h"
#include "engine/runtime/facilities/physics3D.h"
#include "engine/runtime/facilities/prefabs.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/rendering/modules/particleModule.h"
#include "engine/runtime/rendering/picking.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/rendering/standardScene.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/sceneManager/sceneManager.h"
#include "engine/runtime/systems/cameraSystem.h"
#include "engine/runtime/systems/navigationSystem.h"
#include "engine/runtime/systems/physicsSystem3D.h"
#include "engine/runtime/systems/raycastHit3D.h"

#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <glm/glm/ext/vector_float4.hpp>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>

// The engine's facilities (docs/runtime/facilities): what System, Service,
// GameObject and Scene reach the engine's features through.

using namespace sfs;

namespace
{

struct [[sfs::event_news]] PokeEvent
{
};

// What one place's facilities gave.
struct Seen
{
  int available = 0; // of the eight with a backend (debugDraw aside)
  bool drawing = false;
  std::optional<RaycastHit3D> hit;
};

// The ray straight down onto the origin from 10 m up.
Ray downRay()
{
  Ray ray;
  ray.origin = {0.0f, 0.0f, 10.0f};
  ray.direction = {0.0f, 0.0f, -1.0f};
  return ray;
}

Seen see(facility::Physics3D physics,
         facility::Navigation navigation,
         facility::Graphics graphics,
         facility::Meshes meshes,
         facility::Cameras cameras,
         facility::DebugDraw debugDraw,
         facility::Prefabs prefabs,
         facility::Animation animation,
         facility::Assets assets)
{
  Seen seen;
  seen.available = physics.available() + navigation.available() +
                   graphics.available() + meshes.available() +
                   cameras.available() + prefabs.available() +
                   animation.available() + assets.available();
  seen.drawing = debugDraw.enabled();
  seen.hit = physics.raycast(downRay(), 100.0f);
  return seen;
}

// Looks through its facilities each update.
class Looker : public System
{
public:
  Seen seen;

protected:
  void update(double) override
  {
    seen = see(physics(),
               navigation(),
               graphics(),
               meshes(),
               cameras(),
               debugDraw(),
               prefabs(),
               animation(),
               assets());
  }
};

// Looks through its facilities when it's poked.
class LookingService : public Service
{
public:
  Seen seen;

protected:
  void create() override
  {
    m_pokes = consumer<PokeEvent>().onEvent(
        [this](const PokeEvent&)
        {
          seen = see(physics(),
                     navigation(),
                     graphics(),
                     meshes(),
                     cameras(),
                     debugDraw(),
                     prefabs(),
                     animation(),
                     assets());
        });
  }

private:
  EventConsumer<PokeEvent> m_pokes;
};

// Looks through its facilities each update.
class LookingObject : public GameObject
{
public:
  Seen seen;

protected:
  void onUpdate(double) override
  {
    seen = see(physics(),
               navigation(),
               graphics(),
               meshes(),
               cameras(),
               debugDraw(),
               prefabs(),
               animation(),
               assets());
  }
};

// A running scene with every engine part behind the facilities, and a box
// on the origin.
struct World
{
  AssetStore store;
  testing3d::MockRenderer renderer;
  SceneBackends backends{.assetStore = store, .renderer = &renderer};
  SceneManager manager;
  StandardScene* scene = nullptr;
  PhysicsSystem3D* physics = nullptr;
  Entity box;

  World()
  {
    manager.setSceneBackends(&backends);
    scene = manager.createScene<StandardScene>();
    physics = &scene->addSystem<PhysicsSystem3D>();
    scene->addSystem<NavigationSystem>();
    scene->addSystem<CameraSystem>();
    box = scene->createEntity();
    box.addComponent<TransformComponent>(TransformComponent{{0, 0, 0.5f}});
    box.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{{2.0f, 2.0f, 1.0f}});
  }
  ~World() { manager.destroyAll(); }

  void step() { scene->update(1.0 / 60.0); }
};

// Counts the absent-backend warnings logged while it lives.
struct Warnings
{
  std::size_t count = 0;
  Logger::SinkId sink = Logger::addSink(
      [this](Logger::Level level, const std::string& message)
      {
        if (level == Logger::Level::WARN &&
            message.find("used where there's no") != std::string::npos)
          ++count;
      });
  ~Warnings() { Logger::removeSink(sink); }
};

} // namespace

int main()
{
  TEST("in a scene without the engine parts, every facility's queries should "
       "give empty answers and its commands do nothing, with one warning a "
       "missing part, and none from debugDraw()")
  {
    Warnings warnings;
    AssetStore store;
    Scene scene{0, SceneBackends{.assetStore = store}};
    Entity entity = scene.createEntity();
    scene.flushEntities();

    for (int pass = 0; pass < 3; ++pass)
    {
      const facility::Physics3D physics = scene.physics();
      CHECK(!physics.available());
      CHECK(!physics.raycast(downRay()));
      CHECK(!physics.snapshot());
      CHECK(!physics.hasBody(entity));
      CHECK(physics.revision() == 0u);
      physics.setVelocity(entity, {1.0f, 0.0f, 0.0f});

      const facility::Navigation navigation = scene.navigation();
      CHECK(!navigation.available());
      CHECK(!navigation.isAgent(entity));
      CHECK(navigation.path(entity) == nullptr);
      CHECK(navigation.state(entity) == nav::Fleet::State::Idle);
      CHECK(!navigation.scheduler());
      CHECK(navigation.requestGoal(entity, {}) == 0u);
      CHECK(navigation.settings().parkedAfter ==
            nav::Fleet::Settings{}.parkedAfter);
      navigation.cancel(entity);

      const facility::Graphics graphics = scene.graphics();
      CHECK(!graphics.available());
      CHECK(graphics.module<ParticleModule>() == nullptr);
      CHECK(graphics.stats().drawCalls == 0u);

      const facility::Meshes meshes = scene.meshes();
      CHECK(!meshes.available());
      CHECK(!meshes.find("builtin:cube").valid());
      CHECK(meshes.bounds(MeshHandle{}) == nullptr);
      CHECK(!meshes.pickEntity(downRay()));
      CHECK(!meshes.pickSurface(downRay()));

      const facility::Cameras cameras = scene.cameras();
      CHECK(!cameras.available());
      CHECK(!cameras.active().isValid());
      (void)cameras.screenRay({0.0f, 0.0f});

      const facility::Prefabs prefabs = scene.prefabs();
      CHECK(!prefabs.available());
      CHECK(!prefabs.spawn("asset:none", TransformComponent{}).isValid());

      const facility::Animation animation = scene.animation();
      CHECK(!animation.available());
      CHECK(animation.clips(entity) == nullptr);
      CHECK(animation.length(entity) == 0.0f);
      animation.play(entity, "walk", 0.25f);
      animation.restart(entity);

      const facility::DebugDraw debugDraw = scene.debugDraw();
      CHECK(!debugDraw.enabled());
      debugDraw.line({0, 0, 0}, {1, 0, 0}, 0xffffffffu);
      debugDraw.forceColliders(true);
    }

    // The scene loads assets; a registry with no scene has none.
    CHECK(scene.assets().available());
    Registry bare;
    const facility::Assets assets = facility::Assets::in(&bare);
    const ModelData* model = nullptr;
    CHECK(!assets.available());
    CHECK(assets.requestModel(AssetId{}, &model) ==
              AssetStore::LoadState::Failed &&
          model == nullptr);
    CHECK(assets.requestModel(AssetId{}, &model) ==
          AssetStore::LoadState::Failed);
    CHECK(warnings.count ==
          9u); // cameras: its CameraSystem and its RenderSystem
  }

  TEST("each facility should be there from a System, a Service handler, a "
       "GameObject and a Scene, and a raycast through any of them should be "
       "the physics' own")
  {
    debugFeatures::allow(true);
    World world;
    Looker& looker = world.scene->addSystem<Looker>();
    LookingService& service = world.scene->addService<LookingService>();
    LookingObject& object = world.scene->createObject<LookingObject>();
    world.step();
    world.step();
    world.scene->events().send(PokeEvent{});
    world.step();
    const Seen scene = see(world.scene->physics(),
                           world.scene->navigation(),
                           world.scene->graphics(),
                           world.scene->meshes(),
                           world.scene->cameras(),
                           world.scene->debugDraw(),
                           world.scene->prefabs(),
                           world.scene->animation(),
                           world.scene->assets());
    const std::optional<RaycastHit3D> own =
        world.physics->raycast(downRay(), 100.0f);
    CHECK(own && own->entity == world.box);

    const std::initializer_list<const Seen*> places = {
        &looker.seen, &service.seen, &object.seen, &scene};

    for (const Seen* seen : places)
    {
      CHECK(seen->available == 8);
      CHECK(seen->drawing);
      CHECK(seen->hit && own && seen->hit->entity == own->entity &&
            seen->hit->distance == own->distance);
    }

    CHECK(world.scene->graphics().module<ParticleModule>() != nullptr);
    CHECK(world.scene->meshes().find("builtin:cube").valid());
    const std::optional<PickHit> picked =
        world.scene->meshes().pickEntity(downRay());
    CHECK(picked && picked->entity == world.box);
  }

  TEST("debugDraw() should draw while debug features are on, and nothing "
       "while they're off")
  {
    World world;
    RenderSystem* render = world.scene->tryGetSystem<RenderSystem>();
    CHECK(render != nullptr);

    if (render)
    {
      debugFeatures::allow(true);
      render->debugLines().clear();
      world.scene->debugDraw().line({0, 0, 0}, {1, 0, 0}, 0xffffffffu);
      world.scene->debugDraw().box(
          glm::mat4(1.0f), {0, 0, 0}, {1, 1, 1}, 0xffffffffu);
      CHECK(render->debugLines().lineCount() == 13u);
      debugFeatures::allow(false);
      render->debugLines().clear();
      const facility::DebugDraw off = world.scene->debugDraw();
      CHECK(!off.enabled());
      off.line({0, 0, 0}, {1, 0, 0}, glm::vec4(1.0f));
      off.sphere(glm::mat4(1.0f), {0, 0, 0}, 1.0f, 0xffffffffu);
      CHECK(render->debugLines().empty());
      debugFeatures::allow(true);
    }
  }

  TEST("a physics snapshot should outlive the physics it came from")
  {
    World world;
    world.step();
    const std::shared_ptr<const CollisionWorld3D> snapshot =
        world.scene->physics().snapshot();
    world.scene->removeSystem<PhysicsSystem3D>();
    world.physics = nullptr;
    CHECK(!world.scene->physics().available());
    CHECK(snapshot && snapshot->size() == 1u);
    CHECK(snapshot && snapshot->bodies().front().id == world.box.getId());
  }

  return testing::report("facilitiesTests");
}
