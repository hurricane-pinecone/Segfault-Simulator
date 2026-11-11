#include "../../testHarness.h"
#include "../rendering/mockRenderer.h"

#include <engine/core/components/boxCollider3DComponent.h>
#include <engine/core/components/contactReportComponent.h>
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
#include <atomic>
#include <thread>

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
           std::vector<Contact3D>* ended = nullptr,
           std::vector<Contact3D>* deepened = nullptr)
  {
    for (double t = 0.0; t < seconds; t += kFrame)
    {
      scene.update(kFrame);
      if (deepened)
        deepened->insert(deepened->end(),
                         physics.contactsDeepened().begin(),
                         physics.contactsDeepened().end());
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

  // A kinematic box that reports touching static ones, under `parent`.
  Entity cart(const glm::vec3& position, const Entity& parent = {})
  {
    Entity entity = box(position, {1.0f, 1.0f, 1.0f}, false);
    RigidBody3DComponent body;
    body.kinematic = true;
    body.touchStatic = true;
    entity.addComponent<RigidBody3DComponent>(body);
    if (parent)
      hierarchy::setParentKeepWorld(entity, parent);
    return entity;
  }

  // An entity with a ContactReportComponent and nothing to collide with.
  Entity reporter(const Entity& parent = {})
  {
    Entity entity = scene.createEntity();
    entity.addComponent<TransformComponent>();
    entity.addComponent<ContactReportComponent>();
    if (parent)
      hierarchy::setParent(entity, parent);
    return entity;
  }

  // Runs one frame, adding what `report` heard to the lists.
  void step(const Entity& report,
            std::vector<Contact3D>* begun,
            std::vector<Contact3D>* ended = nullptr)
  {
    scene.update(kFrame);
    const auto& heard = report.getComponent<ContactReportComponent>();
    if (begun)
      begun->insert(begun->end(), heard.begun.begin(), heard.begun.end());
    if (ended)
      ended->insert(ended->end(), heard.ended.begin(), heard.ended.end());
  }

  // Teleports a body to `position`.
  static void place(const Entity& entity, const glm::vec3& position)
  {
    hierarchy::setWorldMatrix(entity, TransformComponent(position).matrix());
  }
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

// A scene running at a game speed the way a game does (several steps a
// frame, none longer than 1/30 s), where a driver moves a body each step
// before the physics and a tally counts its steps and contacts after.
class Driver : public System
{
public:
  float x = 0.0f;

  void setSpeed(double speed)
  {
    registry->setTimeScale(speed);
    registry->setMaxTimeStep(1.0 / 30.0, 32);
  }
  void drive(const Entity& entity, float metresPerSecond)
  {
    m_entity = entity;
    m_speed = metresPerSecond;
    x = entity.getComponent<TransformComponent>().position.x;
  }

protected:
  void update(double deltaTime) override
  {
    if (!m_entity.isValid())
      return;
    x += m_speed * static_cast<float>(deltaTime);
    m_entity.getComponent<TransformComponent>().position.x = x;
  }

private:
  Entity m_entity;
  float m_speed = 0.0f;
};

class Tally : public System
{
public:
  PhysicsSystem3D* physics = nullptr;
  int steps = 0;
  int updates = 0;
  std::vector<Contact3D> begun, ended;

protected:
  void update(double) override
  {
    steps += physics->lastUpdateStats().steps;
    ++updates;
    begun.insert(begun.end(),
                 physics->contactsBegun().begin(),
                 physics->contactsBegun().end());
    ended.insert(ended.end(),
                 physics->contactsEnded().begin(),
                 physics->contactsEnded().end());
  }
};

struct Fast
{
  AssetStore store;
  Scene scene{0, SceneServices{.assetStore = store}};
  Driver& driver = scene.addSystem<Driver>();
  PhysicsSystem3D& physics = scene.addSystem<PhysicsSystem3D>();
  Tally& tally = scene.addSystem<Tally>();

  explicit Fast(double speed)
  {
    driver.setSpeed(speed);
    tally.physics = &physics;
  }

  // A kinematic body that reports touching static ones.
  Entity bar(const glm::vec3& position, const glm::vec3& size)
  {
    Entity entity = scene.createEntity();
    entity.addComponent<TransformComponent>(TransformComponent{position});
    entity.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{size, glm::vec3(0.0f), false});
    RigidBody3DComponent body;
    body.kinematic = true;
    body.touchStatic = true;
    entity.addComponent<RigidBody3DComponent>(body);
    return entity;
  }

  // A thin static post, 5 cm square.
  Entity post(const glm::vec3& position)
  {
    Entity entity = scene.createEntity();
    entity.addComponent<TransformComponent>(TransformComponent{position});
    entity.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{{0.05f, 0.05f, 1.0f}, glm::vec3(0.0f), false});
    return entity;
  }

  // A dynamic box resting on its own floor.
  void crate(const glm::vec3& position)
  {
    Entity floor = scene.createEntity();
    floor.addComponent<TransformComponent>(
        TransformComponent{position - glm::vec3(0.0f, 0.0f, 1.0f)});
    floor.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{{4.0f, 4.0f, 1.0f}, glm::vec3(0.0f), false});
    Entity crate = scene.createEntity();
    crate.addComponent<TransformComponent>(TransformComponent{position});
    crate.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{{1.0f, 1.0f, 1.0f}, glm::vec3(0.0f), false});
    crate.addComponent<RigidBody3DComponent>();
  }

  // Frames of 1/144 s.
  void frames(int count)
  {
    for (int i = 0; i < count; ++i)
      scene.update(1.0 / 144.0);
  }
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

  TEST("a snapshot should answer overlaps exactly, turned, by layer, leaving "
       "out what it's told to")
  {
    World world;
    Entity post = world.box({0.0f, 0.0f, 0.6f}, {0.2f, 0.2f, 1.2f}, false);
    Entity wall = world.box({5.0f, 0.0f, 1.0f}, {0.2f, 4.0f, 2.0f}, false);
    wall.getComponent<BoxCollider3DComponent>().layer = 3;
    world.run(kFrame);
    const auto frozen = world.physics.snapshot();
    CHECK(frozen->size() == 2);
    // A 2 x 1 x 1 box standing on the ground.
    const CollisionShape3D box{
        CollisionShape3D::Kind::Box, {2.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.5f}};
    const CollisionWorld3D::Probe probe(std::span(&box, 1));
    // Its side 1 cm off the post's side, then 1 cm into it.
    CHECK(!frozen->overlaps(
        probe, {1.11f + 0.01f, 0.0f, 0.0f}, yawRotation(0.0f)));
    CHECK(frozen->overlaps(probe, {1.09f, 0.0f, 0.0f}, yawRotation(0.0f)));
    // Turned square, its end is only half a metre from its middle.
    CHECK(
        !frozen->overlaps(probe, {0.62f, 0.0f, 0.0f}, yawRotation(1.5707963f)));
    // Over the post's top (1.2 m): clear.
    CHECK(!frozen->overlaps(probe, {0.0f, 0.0f, 1.25f}, yawRotation(0.0f)));
    // The wall is on layer 3: counted only when asked for.
    CHECK(frozen->overlaps(probe, {4.5f, 0.0f, 0.0f}, yawRotation(0.0f)));
    CHECK(!frozen->overlaps(
        probe, {4.5f, 0.0f, 0.0f}, yawRotation(0.0f), ~(1u << 3)));
    const uint64_t skip[] = {wall.getId()};
    CHECK(!frozen->overlaps(
        probe, {4.5f, 0.0f, 0.0f}, yawRotation(0.0f), ~0u, skip));
    const auto hit =
        frozen->overlap(probe, {1.0f, 0.0f, 0.0f}, yawRotation(0.0f));
    CHECK(hit && hit->id == post.getId() && hit->depth > 0.05f);
    // Swept along x from -3 m: it meets the post's side after 1.89 of 6 m.
    const auto along = frozen->cast(
        probe, {-3.0f, 0.0f, 0.0f}, yawRotation(0.0f), {3.0f, 0.0f, 0.0f});
    CHECK(along && testing::approx(*along * 6.0f, 1.89f, 0.02f));
  }

  TEST("a snapshot should stay as it was when the scene changes, and be "
       "queried from several threads at once")
  {
    World world;
    Entity post = world.box({0.0f, 0.0f, 0.5f}, {0.5f, 0.5f, 1.0f}, false);
    world.run(kFrame);
    const uint64_t before = world.physics.revision();
    const auto frozen = world.physics.snapshot();
    hierarchy::setWorldMatrix(
        post, TransformComponent({10.0f, 0.0f, 0.5f}).matrix());
    world.run(kFrame);
    CHECK(world.physics.revision() != before);
    const CollisionShape3D ball{
        CollisionShape3D::Kind::Sphere, {0.3f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f}};
    const CollisionWorld3D::Probe probe(std::span(&ball, 1));
    CHECK(frozen->overlaps(probe, {0.0f, 0.0f, 0.0f}, yawRotation(0.0f)));
    CHECK(world.physics.snapshot()->overlaps(
        probe, {10.0f, 0.0f, 0.0f}, yawRotation(0.0f)));
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
      threads.emplace_back(
          [&, t]
          {
            for (int i = 0; i < 2000; ++i)
            {
              const float x =
                  -2.0f + 0.002f * static_cast<float>(i) + 0.0001f * t;
              const bool expect = std::abs(x) < 0.25f + 0.3f;
              if (frozen->overlaps(probe, {x, 0.0f, 0.0f}, yawRotation(0.0f)) !=
                  expect)
                ++wrong;
            }
          });
    for (std::thread& thread : threads)
      thread.join();
    CHECK(wrong.load() == 0);
  }

  TEST("a kinematic body that asks to should begin and end contacts with "
       "static ones, and say where")
  {
    World world;
    Entity post = world.box({2.0f, 0.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
    Entity cart = world.box({0.0f, 0.0f, 0.5f}, {1.0f, 1.0f, 1.0f}, false);
    RigidBody3DComponent body;
    body.kinematic = true;
    body.touchStatic = true;
    cart.addComponent<RigidBody3DComponent>(body);
    std::vector<Contact3D> begun, ended;
    world.run(kFrame);
    for (int i = 0; i <= 40; ++i)
    {
      hierarchy::setWorldMatrix(
          cart, TransformComponent({0.05f * i, 0.0f, 0.5f}).matrix());
      world.run(kFrame, &begun, &ended);
    }
    const auto touched = std::find_if(begun.begin(),
                                      begun.end(),
                                      [&](const Contact3D& c)
                                      { return involves(c, post, cart); });
    CHECK(touched != begun.end());
    CHECK(touched != begun.end() && std::abs(touched->point.x - 1.9f) < 0.15f);
    for (int i = 0; i <= 40; ++i)
    {
      hierarchy::setWorldMatrix(
          cart, TransformComponent({2.0f - 0.1f * i, 0.0f, 0.5f}).matrix());
      world.run(kFrame, nullptr, &ended);
    }
    CHECK(std::any_of(ended.begin(),
                      ended.end(),
                      [&](const Contact3D& c)
                      { return involves(c, post, cart); }));
  }

  TEST("a kinematic body passing close by should begin a contact only if it "
       "touches, not within the speculative distance")
  {
    // (Jolt reports shapes up to 2 cm apart as speculative contacts.)
    for (float gap : {0.005f, 0.015f, 0.05f, -0.01f})
    {
      World world;
      Entity post = world.box({2.0f, 0.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
      Entity cart =
          world.box({0.0f, 0.6f + gap, 0.5f}, {1.0f, 1.0f, 1.0f}, false);
      RigidBody3DComponent body;
      body.kinematic = true;
      body.touchStatic = true;
      cart.addComponent<RigidBody3DComponent>(body);
      std::vector<Contact3D> begun, ended;
      world.run(kFrame);
      for (int i = 0; i <= 80; ++i)
      {
        hierarchy::setWorldMatrix(
            cart, TransformComponent({0.05f * i, 0.6f + gap, 0.5f}).matrix());
        world.run(kFrame, &begun, &ended);
      }
      const auto with = [&](const std::vector<Contact3D>& list)
      {
        return std::count_if(list.begin(),
                             list.end(),
                             [&](const Contact3D& c)
                             { return involves(c, post, cart); });
      };
      CHECK(with(begun) == (gap < 0.0f ? 1 : 0));
      CHECK(with(ended) == with(begun));
    }
  }

  TEST("a contact should tell how deep it went: began shallow, deepened to "
       "its deepest, ended with that")
  {
    World world;
    Entity post = world.box({2.0f, 0.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
    Entity cart = world.box({2.0f, 0.65f, 0.5f}, {1.0f, 1.0f, 1.0f}, false);
    RigidBody3DComponent body;
    body.kinematic = true;
    body.touchStatic = true;
    cart.addComponent<RigidBody3DComponent>(body);
    std::vector<Contact3D> begun, ended, deepened;
    world.run(kFrame);
    // (In from 5 cm away to 8 cm deep, 5 mm a frame, then back out.)
    const auto place = [&](float y)
    {
      hierarchy::setWorldMatrix(
          cart, TransformComponent({2.0f, y, 0.5f}).matrix());
      world.run(kFrame, &begun, &ended, &deepened);
    };
    for (float y = 0.65f; y >= 0.52f - 1e-4f; y -= 0.005f)
      place(y);
    for (float y = 0.52f; y <= 0.7f; y += 0.005f)
      place(y);
    const auto with = [&](const std::vector<Contact3D>& list)
    {
      std::vector<Contact3D> found;
      for (const Contact3D& c : list)
        if (involves(c, post, cart))
          found.push_back(c);
      return found;
    };
    const std::vector<Contact3D> b = with(begun), e = with(ended),
                                 d = with(deepened);
    CHECK(b.size() == 1);
    CHECK(e.size() == 1);
    CHECK(!d.empty());
    if (b.size() == 1 && e.size() == 1 && !d.empty())
    {
      CHECK(b[0].depth < 0.01f);
      CHECK(std::abs(d.back().depth - 0.08f) < 0.01f);
      CHECK(std::abs(e[0].depth - d.back().depth) < 1e-6f);
      for (size_t i = 1; i < d.size(); ++i)
        CHECK(d[i].depth > d[i - 1].depth);
    }
  }

  TEST("with nothing changing, updates should neither search the scene nor "
       "step")
  {
    World world;
    for (int i = 0; i < 30; ++i)
      world.box({3.0f * i, 0.0f, 0.5f}, {0.5f, 0.5f, 1.0f}, false);
    Entity cart = world.box({0.0f, 10.0f, 0.5f}, {1.0f, 1.0f, 1.0f}, false);
    RigidBody3DComponent body;
    body.kinematic = true;
    cart.addComponent<RigidBody3DComponent>(body);
    world.run(kFrame);
    CHECK(world.physics.lastUpdateStats().lookedForBodies);
    world.run(kFrame);
    int searches = 0, steps = 0;
    for (int i = 0; i < 30; ++i)
    {
      world.run(kFrame);
      searches += world.physics.lastUpdateStats().lookedForBodies ? 1 : 0;
      steps += world.physics.lastUpdateStats().steps;
    }
    CHECK(searches == 0);
    CHECK(steps == 0);

    // Still noticed: a static body moved by code, and a new one.
    const uint64_t revision = world.physics.revision();
    Entity post = world.box({0.0f, 20.0f, 0.5f}, {0.5f, 0.5f, 1.0f}, false);
    world.run(kFrame);
    CHECK(world.physics.lastUpdateStats().lookedForBodies);
    CHECK(world.physics.hasBody(post));
    post.getComponent<TransformComponent>().position.x = 5.0f;
    world.run(kFrame);
    CHECK(!world.physics.lastUpdateStats().lookedForBodies);
    CHECK(world.physics.revision() > revision + 1);
    const auto hit =
        world.physics.raycast(Ray{{5.0f, 20.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
    CHECK(hit && hit->entity == post);
    // And a collider changed in place.
    post.getComponent<BoxCollider3DComponent>().size = {0.5f, 0.5f, 3.0f};
    world.run(kFrame);
    const auto taller =
        world.physics.raycast(Ray{{5.0f, 20.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
    CHECK(taller && testing::approx(taller->point.z, 2.0f, 0.01f));
  }

  TEST("with no dynamic bodies, a slow kinematic body should cost about 60 "
       "steps a second at any game speed")
  {
    for (const double speed : {1.0, 16.0})
    {
      Fast fast(speed);
      Entity cart = fast.bar({0.0f, 0.0f, 0.5f}, {2.0f, 2.0f, 1.0f});
      fast.driver.drive(cart, 0.5f);
      fast.frames(144); // a second
      CHECK(fast.tally.steps >= 40);
      CHECK(fast.tally.steps <= 80); // (16x: was 960)
      CHECK(fast.tally.updates >= (speed > 1.0 ? 144 * 4 : 144));
    }
  }

  TEST("a dynamic body should keep the fixed steps of 60 a second")
  {
    World world;
    world.ground();
    world.box({0.0f, 0.0f, 3.0f}, {1.0f, 1.0f, 1.0f}, true);
    world.scene.update(1.0 / 60.0); // (just short of a step)
    int steps = 0;
    for (int i = 0; i < 10; ++i)
    {
      world.scene.update(1.0 / 60.0);
      steps += world.physics.lastUpdateStats().steps;
    }
    CHECK(steps == 10);
    steps = 0;
    for (int i = 0; i < 20; ++i)
    {
      world.scene.update(1.0 / 120.0);
      steps += world.physics.lastUpdateStats().steps;
    }
    CHECK(steps >= 9 && steps <= 11);
  }

  TEST("a fast kinematic body should begin and end a contact with every thin "
       "post it passes, at any game speed")
  {
    for (const bool dynamics : {false, true})
      for (const double speed : {1.0, 2.0, 16.0})
      {
        Fast fast(speed);
        std::vector<Entity> posts;
        for (int i = 0; i < 20; ++i)
          posts.push_back(fast.post({1.0f * i, 0.0f, 0.5f}));
        if (dynamics) // (fixed steps, far off)
          fast.crate({0.0f, 100.0f, 0.5f});
        Entity bar = fast.bar({-2.0f, 0.0f, 0.5f}, {0.3f, 2.0f, 1.0f});
        fast.driver.drive(bar, 10.0f);
        fast.frames(static_cast<int>(144.0 * 2.5 / speed) + 4);
        CHECK(fast.driver.x > 21.0f);
        int missed = 0, unended = 0, twice = 0;
        for (const Entity& post : posts)
        {
          const auto touching = [&](const Contact3D& c)
          { return involves(c, post, bar); };
          const auto began = std::count_if(
              fast.tally.begun.begin(), fast.tally.begun.end(), touching);
          missed += began == 0 ? 1 : 0;
          twice += began > 1 ? 1 : 0;
          unended +=
              std::none_of(
                  fast.tally.ended.begin(), fast.tally.ended.end(), touching)
                  ? 1
                  : 0;
        }
        CHECK(missed == 0);
        CHECK(twice == 0);
        CHECK(unended == 0);
      }
  }

  TEST("turning a kinematic body shouldn't rebuild it or restart its "
       "contacts")
  {
    World world;
    Entity post = world.box({0.0f, 0.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
    Entity spinner = world.box({0.0f, 0.0f, 0.5f}, {3.0f, 0.5f, 1.0f}, false);
    RigidBody3DComponent body;
    body.kinematic = true;
    body.touchStatic = true;
    spinner.addComponent<RigidBody3DComponent>(body);
    std::vector<Contact3D> begun, ended;
    world.run(kFrame, &begun, &ended);
    world.run(kFrame, &begun, &ended);
    const uint64_t revision = world.physics.revision();
    for (int i = 0; i < 200; ++i)
    {
      spinner.getComponent<TransformComponent>().setYaw(0.037f * i);
      world.run(kFrame, &begun, &ended);
    }
    CHECK(world.physics.revision() == revision);
    CHECK(std::count_if(begun.begin(),
                        begun.end(),
                        [&](const Contact3D& c)
                        { return involves(c, post, spinner); }) == 1);
    CHECK(ended.empty());
  }

  TEST("a contact report on a parent should hear its child collider begin and "
       "end a contact, with `a` set to the child")
  {
    World world;
    Entity post = world.box({3.0f, 0.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
    Entity root = world.reporter();
    Entity cart = world.cart({0.0f, 0.0f, 0.5f}, root);
    std::vector<Contact3D> begun, ended;
    world.step(root, &begun, &ended);
    World::place(cart, {3.0f, 0.0f, 0.5f});
    world.step(root, &begun, &ended);
    CHECK(begun.size() == 1);
    CHECK(!begun.empty() && begun[0].a == cart && begun[0].b == post &&
          !begun[0].trigger);
    World::place(cart, {0.0f, 0.0f, 0.5f});
    world.step(root, nullptr, &ended);
    CHECK(ended.size() == 1);
    CHECK(!ended.empty() && ended[0].a == cart && ended[0].b == post);
  }

  TEST("a nested contact report should take its own subtree from the one "
       "above it")
  {
    World world;
    world.box({3.0f, 0.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
    world.box({3.0f, 5.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
    Entity root = world.reporter();
    Entity inner = world.reporter(root);
    Entity outerCart = world.cart({0.0f, 0.0f, 0.5f}, root);
    Entity innerCart = world.cart({0.0f, 5.0f, 0.5f}, inner);
    std::vector<Contact3D> rootHeard, innerHeard;
    world.step(root, &rootHeard);
    World::place(outerCart, {3.0f, 0.0f, 0.5f});
    World::place(innerCart, {3.0f, 5.0f, 0.5f});
    world.scene.update(kFrame);
    const auto& rootReport = root.getComponent<ContactReportComponent>();
    const auto& innerReport = inner.getComponent<ContactReportComponent>();
    CHECK(rootReport.begun.size() == 1 && rootReport.begun[0].a == outerCart);
    CHECK(innerReport.begun.size() == 1 && innerReport.begun[0].a == innerCart);
  }

  TEST("a contact report should be cleared by an update with no contacts")
  {
    World world;
    world.box({3.0f, 0.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
    Entity root = world.reporter();
    Entity cart = world.cart({0.0f, 0.0f, 0.5f}, root);
    world.scene.update(kFrame);
    World::place(cart, {3.0f, 0.0f, 0.5f});
    world.scene.update(kFrame);
    const auto& report = root.getComponent<ContactReportComponent>();
    CHECK(report.begun.size() == 1);
    world.scene.update(kFrame);
    CHECK(report.begun.empty() && report.ended.empty() &&
          report.deepened.empty());
  }

  TEST("two reported bodies should each hear the one contact, from their own "
       "side")
  {
    World world;
    Entity post = world.box({3.0f, 0.0f, 0.5f}, {0.2f, 0.2f, 1.0f}, false);
    post.addComponent<ContactReportComponent>();
    Entity cart = world.cart({0.0f, 0.0f, 0.5f});
    cart.addComponent<ContactReportComponent>();
    world.scene.update(kFrame);
    World::place(cart, {3.0f, 0.0f, 0.5f});
    world.scene.update(kFrame);
    const auto& postHeard = post.getComponent<ContactReportComponent>();
    const auto& cartHeard = cart.getComponent<ContactReportComponent>();
    CHECK(postHeard.begun.size() == 1 && postHeard.begun[0].a == post &&
          postHeard.begun[0].b == cart);
    CHECK(cartHeard.begun.size() == 1 && cartHeard.begun[0].a == cart &&
          cartHeard.begun[0].b == post);
  }

  TEST("a contact report should flag a trigger contact")
  {
    World world;
    world.ground();
    Entity zone =
        world.box({0.0f, 0.0f, 3.0f}, {4.0f, 4.0f, 1.0f}, false, true);
    zone.addComponent<ContactReportComponent>();
    Entity ball = world.scene.createEntity();
    ball.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f, 6.0f}});
    ball.addComponent<SphereCollider3DComponent>();
    ball.addComponent<RigidBody3DComponent>();
    std::vector<Contact3D> begun;
    for (double t = 0.0; t < 3.0; t += kFrame)
      world.step(zone, &begun);
    CHECK(begun.size() == 1);
    CHECK(!begun.empty() && begun[0].trigger && begun[0].a == zone &&
          begun[0].b == ball);
  }

  return testing::report("physicsSystem3DTests");
}
