#include "../../testHarness.h"

#include <engine/core/components/boxCollider2D.h>
#include <engine/core/components/rigidBody2DComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/game/gameObject.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/scripting/scriptRegistry.h>
#include <engine/runtime/systems/physicsSystem2D.h>

using namespace sfs;

namespace
{
constexpr double kFrame = 1.0 / 60.0;

struct World
{
  AssetStore store;
  Scene scene{0, SceneServices{.assetStore = store}};
  PhysicsSystem2D& physics = scene.addSystem<PhysicsSystem2D>(&scene);

  // A static box: `half` either side of `centre`.
  Entity wall(glm::vec2 centre, glm::vec2 half, bool trigger = false)
  {
    Entity entity = scene.createEntity();
    entity.addComponent<TransformComponent>(TransformComponent{centre});
    entity.addComponent<BoxCollider2D>(
        BoxCollider2D{half, glm::vec2(0.0f), trigger});
    return entity;
  }

  // A 1 x 1 body.
  Entity body(glm::vec2 centre,
              glm::vec2 velocity,
              glm::vec2 gravity = glm::vec2(0.0f))
  {
    Entity entity = wall(centre, glm::vec2(0.5f));
    entity.addComponent<RigidBody2DComponent>(velocity, gravity);
    return entity;
  }

  void run(double seconds)
  {
    for (double t = 0.0; t < seconds; t += kFrame)
      scene.update(kFrame);
  }
};

glm::vec2 positionOf(const Entity& entity)
{
  return entity.getComponent<TransformComponent>().position2D();
}

class Listener : public GameObject
{
public:
  int entered = 0;
  int exited = 0;
  int triggered = 0;
  int left = 0;
  Entity lastOther;
  glm::vec2 lastNormal{0.0f};

protected:
  void onCollisionEnter2D(const Collision2D& collision) override
  {
    ++entered;
    lastOther = collision.other;
    lastNormal = collision.normal;
  }
  void onCollisionExit2D(const Collision2D&) override { ++exited; }
  void onTriggerEnter2D(const Collision2D& collision) override
  {
    ++triggered;
    lastOther = collision.other;
  }
  void onTriggerExit2D(const Collision2D&) override { ++left; }
};
} // namespace

int main()
{
  TEST("a body should move by its velocity and speed up by its gravity")
  {
    World world;
    Entity mover = world.body({0.0f, 0.0f}, {3.0f, 0.0f}, {0.0f, -10.0f});
    world.scene.update(0.5);

    const auto& body = mover.getComponent<RigidBody2DComponent>();
    CHECK(testing::approx(body.velocity.y, -5.0f));
    CHECK(testing::approx(positionOf(mover).x, 1.5f));
    CHECK(testing::approx(positionOf(mover).y, -2.5f));
    CHECK(testing::approx(body.previousPosition.x, 0.0f));
  }

  TEST("a body should stop flush against a static box and lose that speed")
  {
    World world;
    Entity mover = world.body({0.0f, 0.0f}, {60.0f, 0.0f});
    world.wall({3.0f, 0.0f}, {0.5f, 2.0f}); // its left face at x = 2.5
    world.run(0.5);

    CHECK(testing::approx(positionOf(mover).x, 2.0f));
    CHECK(testing::approx(
        mover.getComponent<RigidBody2DComponent>().velocity.x, 0.0f));
  }

  TEST("a fast body should not pass through a thin wall in one step")
  {
    World world;
    Entity mover = world.body({0.0f, 0.0f}, {6000.0f, 0.0f});
    world.wall({10.0f, 0.0f}, {0.05f, 2.0f});
    world.scene.update(kFrame); // 100 units in one step

    CHECK(testing::approx(positionOf(mover).x, 10.0f - 0.05f - 0.5f));
  }

  TEST("a falling body should land on a floor, rest on it and slide along it")
  {
    World world;
    Entity faller = world.body({0.0f, 3.0f}, {2.0f, 0.0f}, {0.0f, -30.0f});
    world.wall({0.0f, -1.0f}, {20.0f, 1.0f}); // its top at y = 0
    world.run(2.0);

    CHECK(testing::approx(positionOf(faller).y, 0.5f));
    CHECK(positionOf(faller).x >
          3.5f); // still sliding: the floor didn't stop x
    CHECK(testing::approx(
        faller.getComponent<RigidBody2DComponent>().velocity.y, 0.0f));
  }

  TEST("a trigger should block nothing but be told what passes through")
  {
    World world;
    Entity mover = world.body({0.0f, 0.0f}, {6.0f, 0.0f});
    Entity zone = world.wall({3.0f, 0.0f}, {0.5f, 1.0f}, true);
    Listener& listener = world.scene.attachObject<Listener>(mover);
    world.run(2.0);

    CHECK(positionOf(mover).x > 10.0f);
    CHECK(listener.triggered == 1);
    CHECK(listener.left == 1);
    CHECK(listener.lastOther == zone);
    CHECK(listener.entered == 0);
  }

  TEST("both game objects should hear of a collision once, with its normal")
  {
    World world;
    Entity faller = world.body({0.0f, 2.0f}, {0.0f, 0.0f}, {0.0f, -20.0f});
    Entity floor = world.wall({0.0f, -1.0f}, {5.0f, 1.0f});
    Listener& onFaller = world.scene.attachObject<Listener>(faller);
    Listener& onFloor = world.scene.attachObject<Listener>(floor);
    world.run(1.0);

    CHECK(onFaller.entered == 1); // resting on it stays one contact
    CHECK(onFaller.lastOther == floor);
    CHECK(testing::approx(onFaller.lastNormal.y, 1.0f)); // pushed up
    CHECK(onFloor.entered == 1);
    CHECK(testing::approx(onFloor.lastNormal.y, -1.0f));

    // Jumping off ends it.
    faller.getComponent<RigidBody2DComponent>().velocity.y = 15.0f;
    world.run(0.2);
    CHECK(onFaller.exited == 1);
  }

  TEST("things at different heights should not meet")
  {
    World world;
    Entity mover = world.body({0.0f, 0.0f}, {60.0f, 0.0f});
    Entity high = world.wall({3.0f, 0.0f}, {0.5f, 2.0f});
    high.getComponent<TransformComponent>().position.z = 2.0f;
    world.run(0.5);

    CHECK(positionOf(mover).x > 20.0f);
  }

  TEST("a child body should move in the world and keep its parent")
  {
    World world;
    Entity rig = world.scene.createEntity();
    rig.addComponent<TransformComponent>(TransformComponent{glm::vec2(5.0f)});
    Entity child = world.body({0.0f, 0.0f}, {60.0f, 0.0f});
    hierarchy::setParent(child, rig);
    world.wall({8.0f, 5.0f}, {0.5f, 2.0f}); // in the world: left face at 7.5
    world.run(0.5);

    CHECK(testing::approx(hierarchy::worldPosition(child).x, 7.0f));
    CHECK(testing::approx(positionOf(child).x, 2.0f));
  }

  TEST("a scene should get 2D physics by listing the system")
  {
    AssetStore store;
    Scene scene{0, SceneServices{.assetStore = store}};
    startScripts(scene, {"PhysicsSystem2D"});
    CHECK(scene.tryGetSystem<PhysicsSystem2D>() != nullptr);
  }

  return testing::report("physicsSystem2DTests");
}
