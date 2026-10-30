#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/components/boxCollider3DComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/rigidBody3DComponent.h>
#include <engine/core/components/sphereCollider3DComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/game/gameObject.h>
#include <engine/runtime/rendering/standardScene.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/sceneManager/sceneManager.h>
#include <engine/runtime/scripting/scriptRegistry.h>
#include <engine/runtime/systems/physicsSystem3D.h>

#include <algorithm>

using namespace sfs;

namespace
{
constexpr double kFrame = 1.0 / 60.0;

struct World
{
  AssetStore store;
  Scene scene{0, SceneServices{.assetStore = store}};
  PhysicsSystem3D& physics = scene.addSystem<PhysicsSystem3D>();

  Entity box(const glm::vec3& position,
             const glm::vec3& size,
             bool dynamic,
             bool trigger = false)
  {
    Entity entity = scene.createEntity();
    entity.addComponent<TransformComponent>(TransformComponent{position});
    entity.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{size, glm::vec3(0.0f), trigger});
    if (dynamic)
      entity.addComponent<RigidBody3DComponent>();
    return entity;
  }

  // A 20 x 20 floor whose top is at z = 0.
  Entity ground()
  {
    return box({0.0f, 0.0f, -0.5f}, {20.0f, 20.0f, 1.0f}, false);
  }

  // Run for a while, collecting every contact that began or ended.
  void run(double seconds,
           std::vector<Contact3D>* begun = nullptr,
           std::vector<Contact3D>* ended = nullptr)
  {
    for (double t = 0.0; t < seconds; t += kFrame)
    {
      scene.update(kFrame);
      if (begun)
        begun->insert(begun->end(),
                      physics.contactsBegun().begin(),
                      physics.contactsBegun().end());
      if (ended)
        ended->insert(ended->end(),
                      physics.contactsEnded().begin(),
                      physics.contactsEnded().end());
    }
  }

  float z(const Entity& entity) { return hierarchy::worldPosition(entity).z; }
};

bool involves(const Contact3D& contact, const Entity& a, const Entity& b)
{
  return (contact.a == a && contact.b == b) ||
         (contact.a == b && contact.b == a);
}

// Records the collision callbacks it gets.
class Listener : public GameObject
{
public:
  int collisions = 0;
  int triggers = 0;
  Entity lastOther;

protected:
  void onCollisionEnter(const Collision3D& collision) override
  {
    ++collisions;
    lastOther = collision.other;
  }
  void onTriggerEnter(const Collision3D&) override { ++triggers; }
};
} // namespace

int main()
{
  TEST("a dynamic box should fall and come to rest on static ground")
  {
    World world;
    world.ground();
    Entity crate = world.box({0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, true);
    world.run(0.5);
    CHECK(world.z(crate) < 5.0f); // falling
    world.run(3.0);
    CHECK(testing::approx(
        world.z(crate), 0.5f, 0.03f)); // its bottom on the floor
  }

  TEST("touching should begin a contact once, and parting should end it")
  {
    World world;
    Entity floor = world.ground();
    Entity crate = world.box({0.0f, 0.0f, 2.0f}, {1.0f, 1.0f, 1.0f}, true);
    std::vector<Contact3D> begun, ended;
    world.run(2.0, &begun, &ended);
    CHECK(std::count_if(begun.begin(),
                        begun.end(),
                        [&](const Contact3D& c)
                        { return involves(c, floor, crate); }) == 1);
    CHECK(!begun.empty() && !begun.front().trigger);

    world.physics.setVelocity(crate, {0.0f, 0.0f, 8.0f}); // jump off
    world.run(0.3, nullptr, &ended);
    CHECK(std::any_of(ended.begin(),
                      ended.end(),
                      [&](const Contact3D& c)
                      { return involves(c, floor, crate); }));
  }

  TEST("a trigger should report what passes through without stopping it")
  {
    World world;
    world.ground();
    Entity zone =
        world.box({0.0f, 0.0f, 3.0f}, {4.0f, 4.0f, 1.0f}, false, true);
    Entity ball = world.scene.createEntity();
    ball.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f, 6.0f}});
    ball.addComponent<SphereCollider3DComponent>();
    ball.addComponent<RigidBody3DComponent>();

    std::vector<Contact3D> begun, ended;
    world.run(3.0, &begun, &ended);
    const auto withZone = [&](const Contact3D& c)
    { return involves(c, zone, ball) && c.trigger; };
    CHECK(std::count_if(begun.begin(), begun.end(), withZone) == 1);
    CHECK(std::count_if(ended.begin(), ended.end(), withZone) == 1);
    CHECK(testing::approx(
        world.z(ball), 0.5f, 0.05f)); // fell through it to the floor
  }

  TEST("a raycast should hit the nearest solid body, skipping triggers")
  {
    World world;
    Entity floor = world.ground();
    world.box({0.0f, 0.0f, 4.0f},
              {2.0f, 2.0f, 1.0f},
              false,
              true); // a trigger in the way
    world.run(kFrame);

    const auto hit =
        world.physics.raycast(Ray{{0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, -1.0f}});
    CHECK(hit.has_value());
    CHECK(hit->entity == floor);
    CHECK(testing::approx(hit->distance, 10.0f, 1e-3f));
    CHECK(testing::approx(hit->normal.z, 1.0f, 1e-3f));
    CHECK(!world.physics.raycast(Ray{{50.0f, 0.0f, 10.0f}, {0.0f, 0.0f, -1.0f}})
               .has_value());
  }

  TEST("moving a body's transform from code should move the body")
  {
    World world;
    Entity wall = world.box({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false);
    world.run(kFrame);
    wall.getComponent<TransformComponent>().position = {10.0f, 0.0f, 0.0f};
    world.run(kFrame);
    const auto hit =
        world.physics.raycast(Ray{{10.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
    CHECK(hit.has_value() && hit->entity == wall);
  }

  TEST("a kinematic body should follow its transform and not fall")
  {
    World world;
    Entity lift = world.box({0.0f, 0.0f, 2.0f}, {2.0f, 2.0f, 0.2f}, true);
    lift.getComponent<RigidBody3DComponent>().kinematic = true;
    world.run(1.0);
    CHECK(testing::approx(world.z(lift), 2.0f));
    lift.getComponent<TransformComponent>().position.z = 3.0f;
    world.run(0.2);
    CHECK(testing::approx(world.z(lift), 3.0f));
  }

  TEST("a dynamic child should land where physics puts it, relative to its "
       "parent")
  {
    World world;
    world.ground();
    Entity rack = world.scene.createEntity();
    rack.addComponent<TransformComponent>(
        TransformComponent{{5.0f, 0.0f, 0.0f}});
    Entity crate = world.box({0.0f, 0.0f, 3.0f}, {1.0f, 1.0f, 1.0f}, true);
    hierarchy::setParent(crate, rack);
    world.run(3.0);
    CHECK(testing::approx(hierarchy::worldPosition(crate).x, 5.0f, 0.02f));
    CHECK(testing::approx(world.z(crate), 0.5f, 0.03f));
    CHECK(testing::approx(crate.getComponent<TransformComponent>().position.x,
                          0.0f,
                          0.02f)); // local
  }

  TEST("velocity and impulses should move a dynamic body")
  {
    World world;
    world.physics.setGravity(glm::vec3(0.0f));
    Entity puck = world.box({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
    puck.getComponent<RigidBody3DComponent>().linearDamping = 0.0f;
    world.run(kFrame);
    world.physics.setVelocity(puck, {2.0f, 0.0f, 0.0f});
    world.run(1.0);
    CHECK(testing::approx(hierarchy::worldPosition(puck).x, 2.0f, 0.1f));
    CHECK(testing::approx(world.physics.velocity(puck).x, 2.0f, 1e-3f));
    world.physics.addImpulse(puck, {0.0f, 3.0f, 0.0f}); // mass 1: +3 m/s
    world.run(kFrame);
    CHECK(testing::approx(world.physics.velocity(puck).y, 3.0f, 1e-2f));
  }

  TEST("removing an entity's colliders or the entity should remove its body")
  {
    World world;
    Entity a = world.box({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false);
    Entity b = world.box({3.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false);
    world.run(kFrame);
    CHECK(world.physics.hasBody(a) && world.physics.hasBody(b));
    a.removeComponent<BoxCollider3DComponent>();
    world.scene.destroyEntity(b);
    world.run(kFrame);
    CHECK(!world.physics.hasBody(a));
    CHECK(!world.physics.hasBody(b));
  }

  TEST("a disabled physics system should leave everything where it is")
  {
    World world;
    Entity crate = world.box({0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, true);
    world.physics.setEnabled(false); // the editor, while editing
    world.run(1.0);
    CHECK(testing::approx(world.z(crate), 5.0f));
    CHECK(!world.physics.hasBody(crate));
  }

  TEST("a StandardScene should have physics only when it lists the system")
  {
    AssetStore store;
    testing3d::MockRenderer renderer;
    SceneServices services{.assetStore = store, .renderer = &renderer};
    SceneManager manager;
    manager.setSceneServices(&services);
    StandardScene& scene = *manager.createScene<StandardScene>();
    CHECK(scene.physicsSystem() == nullptr);
    startScripts(scene, {"PhysicsSystem3D"});
    CHECK(scene.physicsSystem() != nullptr);
  }

  TEST("game objects on a StandardScene should hear about their collisions and "
       "triggers")
  {
    AssetStore store;
    testing3d::MockRenderer renderer;
    SceneServices services{.assetStore = store, .renderer = &renderer};
    SceneManager manager;
    manager.setSceneServices(&services);
    StandardScene& scene = *manager.createScene<StandardScene>();
    startScripts(
        scene, {"PhysicsSystem3D"}); // as when a scene lists it and plays

    Entity floor = scene.createEntity();
    floor.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f, -0.5f}});
    floor.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{{20.0f, 20.0f, 1.0f}});
    Entity zone = scene.createEntity();
    zone.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f, 2.0f}});
    zone.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{{3.0f, 3.0f, 0.5f}, {}, true});
    Entity crate = scene.createEntity();
    crate.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f, 4.0f}});
    crate.addComponent<BoxCollider3DComponent>();
    crate.addComponent<RigidBody3DComponent>();
    Listener& listener = scene.attachObject<Listener>(crate);

    for (int i = 0; i < 180; ++i)
      scene.update(kFrame);
    CHECK(listener.triggers == 1);
    CHECK(listener.collisions == 1);
    CHECK(listener.lastOther == floor);
  }

  return testing::report("physicsSystem3DTests");
}
