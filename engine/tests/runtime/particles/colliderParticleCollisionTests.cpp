#include "../../testHarness.h"

#include <engine/core/components/boxCollider2D.h>
#include <engine/core/components/tags/solidObject.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/particles/iParticleCollisionSource.h>
#include <engine/runtime/particles/colliderParticleCollision.h>

using namespace sfs;

namespace
{
// A solid box centred at `pos` with the given half-extents, tagged so the sweep
// considers it.
Entity addSolidBox(Registry& reg, glm::vec2 pos, glm::vec2 half)
{
  Entity e = reg.createEntity();
  e.addComponent<TransformComponent>(TransformComponent{pos});
  e.addComponent<BoxCollider2D>(BoxCollider2D{half});
  e.addTag<SolidObject>();
  return e;
}

ParticleSweep segment(glm::vec2 from, glm::vec2 to)
{
  ParticleSweep s;
  s.from = from;
  s.to = to;
  return s;
}
} // namespace

int main()
{
  TEST("a sweep into a solid should report the entry contact")
  {
    Registry reg;
    addSolidBox(reg, {5.0f, 0.0f}, {1.0f, 1.0f}); // box spans x[4,6], y[-1,1]
    reg.flushEntities();
    ColliderParticleCollision coll(&reg);

    const ParticleHit hit = coll.sweep(segment({0.0f, 0.0f}, {10.0f, 0.0f}));
    CHECK(hit.hit);
    CHECK(testing::approx(hit.pos.x, 4.0f)); // enters at the left face
    CHECK(testing::approx(hit.normal.x, -1.0f));
    CHECK(testing::approx(hit.boundsMin.x, 4.0f));
    CHECK(testing::approx(hit.boundsMax.x, 6.0f));
  }

  TEST("a sweep that misses every solid should not hit")
  {
    Registry reg;
    addSolidBox(reg, {5.0f, 0.0f}, {1.0f, 1.0f});
    reg.flushEntities();
    ColliderParticleCollision coll(&reg);

    const ParticleHit hit = coll.sweep(segment({0.0f, 5.0f}, {10.0f, 5.0f}));
    CHECK(!hit.hit);
  }

  TEST("the nearest solid along the sweep should win")
  {
    Registry reg;
    addSolidBox(reg, {8.0f, 0.0f}, {1.0f, 1.0f}); // farther
    addSolidBox(reg, {5.0f, 0.0f}, {1.0f, 1.0f}); // nearer (entry x=4)
    reg.flushEntities();
    ColliderParticleCollision coll(&reg);

    const ParticleHit hit = coll.sweep(segment({0.0f, 0.0f}, {10.0f, 0.0f}));
    CHECK(hit.hit);
    CHECK(testing::approx(hit.pos.x, 4.0f)); // the nearer box, not the x=7 one
  }

  TEST("a sweep with no registry should not hit")
  {
    ColliderParticleCollision coll(nullptr);
    const ParticleHit hit = coll.sweep(segment({0.0f, 0.0f}, {10.0f, 0.0f}));
    CHECK(!hit.hit);
  }

  return testing::report("colliderParticleCollisionTests");
}
