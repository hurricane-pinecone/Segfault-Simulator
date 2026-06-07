#include "../../testHarness.h"

#include <engine/core/components/elevationComponent.h>
#include <engine/core/components/rigidBodyComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/components/worldCollider.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/rendering/iTerrainHeightSource.h>
#include <engine/runtime/systems/movementSystem.h>

#include <functional>

using namespace sfs;

namespace
{
// Answers terrain heights from a per-tile function, so each test shapes its own
// cliffs without a real terrain generator.
struct StubTerrain : ITerrainHeightSource
{
  std::function<int(int, int)> heightAt;
  int terrainHeightAt(int x, int y) const override
  {
    return heightAt ? heightAt(x, y) : 0;
  }
};

// A mover with the components MovementSystem matches on, flushed into the
// system.
Entity addMover(Registry& reg, glm::vec2 position, glm::vec2 velocity)
{
  Entity e = reg.createEntity();
  e.addComponent<TransformComponent>(TransformComponent{position});
  e.addComponent<RigidBodyComponent>(RigidBodyComponent{velocity});
  return e;
}
} // namespace

int main()
{
  TEST("without a terrain source an entity should move freely")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    Entity e = addMover(reg, {0.0f, 0.0f}, {10.0f, 4.0f});
    reg.flushEntities();

    sys.update(0.1); // desired = position + velocity * dt

    const auto& t = e.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, 1.0f));
    CHECK(testing::approx(t.position.y, 0.4f));
    CHECK(testing::approx(t.previousPosition.x, 0.0f)); // captured pre-move
  }

  TEST("a tall step should block movement and stop the velocity")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    StubTerrain terrain;
    terrain.heightAt = [](int x, int)
    { return x >= 1 ? 10 : 0; }; // cliff at x=1
    sys.setTerrainHeightSource(&terrain);

    Entity e = addMover(reg, {0.0f, 0.0f}, {10.0f, 0.0f}); // desired x = 1.0
    reg.flushEntities();
    sys.update(0.1);

    const auto& t = e.getComponent<TransformComponent>();
    const auto& rb = e.getComponent<RigidBodyComponent>();
    CHECK(testing::approx(t.position.x, 0.0f)); // held at the cliff edge
    CHECK(testing::approx(rb.velocity.x, 0.0f));
  }

  TEST("a step within the climb limit should be walkable")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    StubTerrain terrain;
    terrain.heightAt = [](int x, int) { return x >= 1 ? 2 : 0; }; // rise of 2
    sys.setTerrainHeightSource(&terrain);

    Entity e = addMover(reg, {0.0f, 0.0f}, {10.0f, 0.0f});
    reg.flushEntities();
    sys.update(0.1);

    CHECK(
        testing::approx(e.getComponent<TransformComponent>().position.x, 1.0f));
  }

  TEST("stepping down any drop should be allowed")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    StubTerrain terrain;
    terrain.heightAt = [](int x, int) { return x >= 1 ? 0 : 20; }; // big drop
    sys.setTerrainHeightSource(&terrain);

    Entity e = addMover(reg, {0.0f, 0.0f}, {10.0f, 0.0f});
    reg.flushEntities();
    sys.update(0.1);

    CHECK(
        testing::approx(e.getComponent<TransformComponent>().position.x, 1.0f));
  }

  TEST("a body should slide along a wall it cannot cross")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    StubTerrain terrain;
    // A wall down the x==1 column only; y stays open.
    terrain.heightAt = [](int x, int) { return x == 1 ? 10 : 0; };
    sys.setTerrainHeightSource(&terrain);

    Entity e =
        addMover(reg, {0.5f, 0.5f}, {10.0f, 10.0f}); // desired (1.5, 1.5)
    reg.flushEntities();
    sys.update(0.1);

    const auto& t = e.getComponent<TransformComponent>();
    const auto& rb = e.getComponent<RigidBodyComponent>();
    CHECK(testing::approx(t.position.x, 0.5f)); // x blocked by the wall
    CHECK(testing::approx(t.position.y, 1.5f)); // y slides past it
    CHECK(testing::approx(rb.velocity.x, 0.0f));
    CHECK(testing::approx(rb.velocity.y, 10.0f));
  }

  TEST("elevation should follow the terrain the entity stands on")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    StubTerrain terrain;
    terrain.heightAt = [](int, int) { return 3; }; // flat plateau at level 3
    sys.setTerrainHeightSource(&terrain);

    Entity e = addMover(reg, {0.0f, 0.0f}, {5.0f, 0.0f});
    e.addComponent<ElevationComponent>(ElevationComponent{0});
    reg.flushEntities();
    sys.update(0.1);

    CHECK(e.getComponent<ElevationComponent>().level == 3);
  }

  TEST("a world collider's bounds should track the moved position")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    Entity e = addMover(reg, {0.0f, 0.0f}, {32.0f, 0.0f}); // one tile in dt=0.1
    e.addComponent<WorldCollider>(
        WorldCollider{glm::vec2{0.0f}, glm::vec2{32.0f}}); // 1x1 tile footprint
    reg.flushEntities();
    sys.update(0.1);

    const auto& collider = e.getComponent<WorldCollider>();
    CHECK(testing::approx(collider.left(), 3.2f));
    CHECK(testing::approx(collider.right(), 4.2f));
  }

  return testing::report("movementSystemTests");
}
