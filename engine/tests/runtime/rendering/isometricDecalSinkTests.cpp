#include "../../testHarness.h"

#include <engine/core/particles/decal.h>
#include <engine/runtime/rendering/modules/isometricDecalSink.h>

using namespace sfs;

namespace
{
const glm::vec4 kRed{0.35f, 0.0f, 0.0f, 1.0f};
const glm::vec4 kBlue{0.0f, 0.0f, 0.35f, 1.0f};

DecalSpawn ground(glm::vec2 pos, glm::vec4 color)
{
  DecalSpawn s;
  s.worldPos = pos;
  s.surface = DecalSurface::Ground;
  s.color = color;
  s.fadeRate = 0.0f; // permanent: counts toward cell saturation
  return s;
}

DecalSpawn wall(glm::vec2 pos, glm::vec4 color)
{
  DecalSpawn s = ground(pos, color);
  s.surface = DecalSurface::Wall;
  s.wallSide = 0;
  s.wallBottom = 0.0f;
  s.wallTop = 4.0f;
  s.dripSpeed = 0.0f; // static impact mark, not a running drip
  return s;
}
} // namespace

int main()
{
  TEST("addDecal should grow the decal count")
  {
    IsometricDecalSink sink;
    CHECK(sink.decalCount() == 0);
    sink.addDecal(ground({0.5f, 0.5f}, kRed));
    CHECK(sink.decalCount() == 1);
  }

  TEST("the same paint should saturate a cell at its quota")
  {
    IsometricDecalSink sink;
    for (int i = 0; i < 30; ++i) // far past the quota, all the same colour/cell
      sink.addDecal(ground({0.5f, 0.5f}, kRed));
    // The default ground quota: extra sprays of an already-painted colour drop.
    CHECK(sink.decalCount() == 16);
  }

  TEST("a different colour should repaint a saturated cell")
  {
    IsometricDecalSink sink;
    for (int i = 0; i < 30; ++i)
      sink.addDecal(ground({0.5f, 0.5f}, kRed));
    sink.addDecal(ground({0.5f, 0.5f}, kBlue)); // distinct hue -> repaint
    CHECK(sink.decalCount() == 1);
  }

  TEST("wall paint should saturate at the lower wall quota")
  {
    IsometricDecalSink sink;
    for (int i = 0; i < 30; ++i)
      sink.addDecal(wall({0.5f, 0.5f}, kRed));
    CHECK(sink.decalCount() == 5); // walls saturate sooner than ground
  }

  TEST("clearAll should drop every decal")
  {
    IsometricDecalSink sink;
    sink.addDecal(ground({0.5f, 0.5f}, kRed));
    sink.addDecal(ground({5.5f, 5.5f}, kBlue));
    sink.clearAll();
    CHECK(sink.decalCount() == 0);
  }

  TEST("clearRegion should drop only decals inside the tile rect")
  {
    IsometricDecalSink sink;
    sink.addDecal(ground({0.5f, 0.5f}, kRed));  // tile (0, 0)
    sink.addDecal(ground({5.5f, 5.5f}, kBlue)); // tile (5, 5)
    sink.clearRegion(glm::ivec2{0, 0}, glm::ivec2{1, 1}); // max is exclusive
    CHECK(sink.decalCount() == 1);
  }

  TEST("a fading decal should age out on update")
  {
    IsometricDecalSink sink;
    DecalSpawn water = ground({0.5f, 0.5f}, kBlue);
    water.surface = DecalSurface::Water;
    water.fadeRate = 1.0f; // fully faded after one second
    sink.addDecal(water);
    CHECK(sink.decalCount() == 1);

    sink.update(1.0);
    CHECK(sink.decalCount() == 0);
  }

  return testing::report("isometricDecalSinkTests");
}
