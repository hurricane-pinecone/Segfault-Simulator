#include "../../testHarness.h"

#include <engine/core/components/elevationComponent.h>
#include <engine/core/components/rigidBodyComponent.h>
#include <engine/core/components/tags/solidObject.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/components/worldCollider.h>
#include <engine/core/ecs/ecs.h>
#include <engine/runtime/systems/collisionSystem.h>

using namespace sfs;

namespace
{
// A 1x1-tile footprint (32px tiles, the WorldCollider default scale).
WorldCollider tileCollider()
{
  return WorldCollider{glm::vec2{0.0f}, glm::vec2{32.0f}};
}

// A moving body, with previousPosition distinct from the current position so the
// solver can tell which edge it crossed.
Entity addBody(Registry& reg, glm::vec2 prev, glm::vec2 pos, glm::vec2 vel)
{
  Entity e = reg.createEntity();
  TransformComponent t{prev}; // previousPosition := prev
  t.position = pos;           // ... then diverge from it
  e.addComponent<TransformComponent>(t);
  e.addComponent<RigidBodyComponent>(RigidBodyComponent{vel});
  e.addComponent<WorldCollider>(tileCollider());
  return e;
}

Entity addSolid(Registry& reg, glm::vec2 pos)
{
  Entity e = reg.createEntity();
  e.addComponent<TransformComponent>(TransformComponent{pos});
  e.addComponent<WorldCollider>(tileCollider());
  e.addTag<SolidObject>();
  return e;
}
} // namespace

int main()
{
  TEST("a rightward body should stop at a solid's left edge")
  {
    Registry reg;
    CollisionSystem& sys = reg.addSystem<CollisionSystem>();
    Entity mover = addBody(reg, {0.0f, 0.0f}, {0.5f, 0.0f}, {5.0f, 0.0f});
    addSolid(reg, {1.0f, 0.0f});
    reg.flushEntities();

    sys.update(0.016);

    const auto& t = mover.getComponent<TransformComponent>();
    const auto& rb = mover.getComponent<RigidBodyComponent>();
    CHECK(testing::approx(t.position.x, 0.0f)); // pushed back flush to the edge
    CHECK(testing::approx(rb.velocity.x, 0.0f));
  }

  TEST("a leftward body should stop at a solid's right edge")
  {
    Registry reg;
    CollisionSystem& sys = reg.addSystem<CollisionSystem>();
    Entity mover = addBody(reg, {2.0f, 0.0f}, {1.5f, 0.0f}, {-5.0f, 0.0f});
    addSolid(reg, {1.0f, 0.0f});
    reg.flushEntities();

    sys.update(0.016);

    const auto& t = mover.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, 2.0f));
    CHECK(testing::approx(mover.getComponent<RigidBodyComponent>().velocity.x,
                          0.0f));
  }

  TEST("a downward body should stop at a solid's top edge")
  {
    Registry reg;
    CollisionSystem& sys = reg.addSystem<CollisionSystem>();
    Entity mover = addBody(reg, {0.0f, 0.0f}, {0.0f, 0.5f}, {0.0f, 5.0f});
    addSolid(reg, {0.0f, 1.0f});
    reg.flushEntities();

    sys.update(0.016);

    const auto& t = mover.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.y, 0.0f));
    CHECK(testing::approx(mover.getComponent<RigidBodyComponent>().velocity.y,
                          0.0f));
  }

  TEST("solids on a different elevation should not block")
  {
    Registry reg;
    CollisionSystem& sys = reg.addSystem<CollisionSystem>();
    Entity mover = addBody(reg, {0.0f, 0.0f}, {0.5f, 0.0f}, {5.0f, 0.0f});
    Entity solid = addSolid(reg, {1.0f, 0.0f});
    solid.addComponent<ElevationComponent>(ElevationComponent{5}); // mover at 0
    reg.flushEntities();

    sys.update(0.016);

    const auto& t = mover.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, 0.5f)); // passes through, unresolved
    CHECK(testing::approx(mover.getComponent<RigidBodyComponent>().velocity.x,
                          5.0f));
  }

  TEST("a stationary body should be skipped by the solver")
  {
    Registry reg;
    CollisionSystem& sys = reg.addSystem<CollisionSystem>();
    Entity mover = addBody(reg, {0.5f, 0.0f}, {0.5f, 0.0f}, {0.0f, 0.0f});
    addSolid(reg, {1.0f, 0.0f}); // overlapping, but the mover isn't moving
    reg.flushEntities();

    sys.update(0.016);

    CHECK(testing::approx(mover.getComponent<TransformComponent>().position.x,
                          0.5f));
  }

  TEST("a collider without the solid tag should not block")
  {
    Registry reg;
    CollisionSystem& sys = reg.addSystem<CollisionSystem>();
    Entity mover = addBody(reg, {0.0f, 0.0f}, {0.5f, 0.0f}, {5.0f, 0.0f});

    Entity blocker = reg.createEntity(); // collider, but no SolidObject tag
    blocker.addComponent<TransformComponent>(TransformComponent{{1.0f, 0.0f}});
    blocker.addComponent<WorldCollider>(tileCollider());
    reg.flushEntities();

    sys.update(0.016);

    const auto& t = mover.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, 0.5f));
    CHECK(testing::approx(mover.getComponent<RigidBodyComponent>().velocity.x,
                          5.0f));
  }

  return testing::report("collisionSystemTests");
}
