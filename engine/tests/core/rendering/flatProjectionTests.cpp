#include "../../testHarness.h"

#include <engine/core/rendering/projection/flatProjection.h>

using namespace sfs;

namespace
{
bool nearVec(const glm::vec2& a, const glm::vec2& b, float eps = 1e-4f)
{
  return testing::approx(a.x, b.x, eps) && testing::approx(a.y, b.y, eps);
}
} // namespace

int main()
{
  TEST("worldToScreen should offset world positions by the screen centre")
  {
    FlatProjection p;
    p.cameraCenter = {0.0f, 0.0f};
    p.screenCenter = {400.0f, 300.0f};
    p.zoom = 1.0f;
    CHECK(nearVec(p.worldToScreen({0.0f, 0.0f}, 0.0f), {400.0f, 300.0f}));
    CHECK(nearVec(p.worldToScreen({10.0f, -5.0f}, 0.0f), {410.0f, 295.0f}));
  }

  TEST("worldToScreen should ignore elevation in 2D")
  {
    FlatProjection p;
    p.cameraCenter = {0.0f, 0.0f};
    p.screenCenter = {400.0f, 300.0f};
    p.zoom = 1.0f;
    CHECK(nearVec(p.worldToScreen({10.0f, -5.0f}, 999.0f), {410.0f, 295.0f}));
  }

  TEST("zoom should scale world units to pixels")
  {
    FlatProjection p;
    p.cameraCenter = {0.0f, 0.0f};
    p.screenCenter = {400.0f, 300.0f};
    p.zoom = 2.0f;
    CHECK(nearVec(p.worldToScreen({10.0f, 0.0f}, 0.0f), {420.0f, 300.0f}));
    CHECK(testing::approx(p.worldUnitToPixels(), 2.0f, 1e-6f));
  }

  TEST("screenToWorld should invert worldToScreen with a panned camera")
  {
    FlatProjection p;
    p.cameraCenter = {123.0f, -45.0f};
    p.screenCenter = {400.0f, 300.0f};
    p.zoom = 1.5f;
    const glm::vec2 world{17.0f, 88.0f};
    CHECK(nearVec(p.screenToWorld(p.worldToScreen(world, 0.0f), 0.0f), world));
  }

  return testing::report("flatProjectionTests");
}
