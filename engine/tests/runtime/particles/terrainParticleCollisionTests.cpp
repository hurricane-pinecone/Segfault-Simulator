#include "../../testHarness.h"

#include <engine/core/particles/decal.h>
#include <engine/core/particles/iParticleCollisionSource.h>
#include <engine/core/rendering/iTerrainSurfaceSource.h>
#include <engine/runtime/particles/terrainParticleCollision.h>

#include <functional>

using namespace sfs;

namespace
{
// A terrain heightfield with optional per-tile water, driven by functions so
// each test shapes its own cliffs.
struct StubSurface : ITerrainSurfaceSource
{
  std::function<int(int, int)> height;
  std::function<bool(int, int)> water;

  int terrainHeightAt(int x, int y) const override
  {
    return height ? height(x, y) : 0;
  }
  bool isWaterAt(int x, int y) const override
  {
    return water ? water(x, y) : false;
  }
};

ParticleSweep sweepAt(glm::vec2 from, glm::vec2 to, float toZ)
{
  ParticleSweep s;
  s.from = from;
  s.to = to;
  s.toZ = toZ;
  return s;
}
} // namespace

int main()
{
  TEST("crossing into a taller tile on a visible face should stick to the wall")
  {
    StubSurface terrain;
    terrain.height = [](int x, int) { return x <= 0 ? 5 : 0; }; // cliff at x=0
    TerrainParticleCollision coll(&terrain);

    // Moving -x into the tall tile presents its east face (a camera-facing
    // side).
    const ParticleHit hit =
        coll.sweep(sweepAt({1.5f, 0.5f}, {0.5f, 0.5f}, 0.0f));
    CHECK(hit.hit);
    CHECK(hit.surface == DecalSurface::Wall);
    CHECK(hit.wallSide == 2); // east
    CHECK(testing::approx(hit.wallBottom, 0.0f));
    CHECK(testing::approx(hit.wallTop, 5.0f));
  }

  TEST("a step up on a hidden face should pool as ground at the base")
  {
    StubSurface terrain;
    terrain.height = [](int x, int) { return x >= 1 ? 5 : 0; };
    TerrainParticleCollision coll(&terrain);

    // Moving +x steps up onto the west face, which is hidden behind the block.
    const ParticleHit hit =
        coll.sweep(sweepAt({0.5f, 0.5f}, {1.5f, 0.5f}, 0.0f));
    CHECK(hit.hit);
    CHECK(hit.surface == DecalSurface::Ground);
    CHECK(testing::approx(hit.elevation, 0.0f)); // the lower base, not the top
  }

  TEST("landing within a tile should stain the ground at its height")
  {
    StubSurface terrain;
    terrain.height = [](int, int) { return 3; };
    TerrainParticleCollision coll(&terrain);

    const ParticleHit hit =
        coll.sweep(sweepAt({0.5f, 0.5f}, {0.6f, 0.6f}, 0.0f));
    CHECK(hit.hit);
    CHECK(hit.surface == DecalSurface::Ground);
    CHECK(testing::approx(hit.elevation, 3.0f));
  }

  TEST("landing on water should mark water with a fade rate")
  {
    StubSurface terrain;
    terrain.height = [](int, int) { return 0; };
    terrain.water = [](int, int) { return true; };
    TerrainParticleCollision coll(&terrain);

    const ParticleHit hit =
        coll.sweep(sweepAt({0.5f, 0.5f}, {0.6f, 0.6f}, 0.0f));
    CHECK(hit.hit);
    CHECK(hit.surface == DecalSurface::Water);
    CHECK(testing::approx(hit.fadeRate, 0.5f)); // the default water fade rate
  }

  TEST("a particle still above the surface should not land")
  {
    StubSurface terrain;
    terrain.height = [](int, int) { return 0; };
    TerrainParticleCollision coll(&terrain);

    const ParticleHit hit = coll.sweep(
        sweepAt({0.5f, 0.5f}, {0.6f, 0.6f}, 5.0f)); // still in the air
    CHECK(!hit.hit);
  }

  TEST("groundElevation should read the tile height under a point")
  {
    StubSurface terrain;
    terrain.height = [](int x, int y) { return (x == 2 && y == 3) ? 7 : 0; };
    TerrainParticleCollision coll(&terrain);

    CHECK(testing::approx(coll.groundElevation({2.5f, 3.5f}), 7.0f));
  }

  TEST("a collision with no terrain should be inert")
  {
    TerrainParticleCollision coll(nullptr);
    CHECK(!coll.sweep(sweepAt({0.0f, 0.0f}, {1.0f, 0.0f}, 0.0f)).hit);
    CHECK(testing::approx(coll.groundElevation({0.0f, 0.0f}), 0.0f));
  }

  return testing::report("terrainParticleCollisionTests");
}
