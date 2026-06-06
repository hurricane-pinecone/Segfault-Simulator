// Dependency-free tests for FlatProjection: the flat 2D world<->screen transform
// the general 2D render path uses. Returns non-zero on any failure (CTest test).

#include <engine/core/rendering/projection/flatProjection.h>

#include <cmath>
#include <cstdio>

namespace
{
int g_failures = 0;
int g_passed = 0;

void check(bool cond, const char* expr, const char* file, int line)
{
  if (cond)
  {
    ++g_passed;
  }
  else
  {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
  }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

bool near(const glm::vec2& a, const glm::vec2& b, float eps = 1e-4f)
{
  return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps;
}
} // namespace

int main()
{
  // Camera centred on the origin, screen centre at (400,300), no zoom: world
  // maps to screen offset by the screen centre.
  sfs::FlatProjection p;
  p.cameraCenter = {0.0f, 0.0f};
  p.screenCenter = {400.0f, 300.0f};
  p.zoom = 1.0f;

  CHECK(near(p.worldToScreen({0.0f, 0.0f}, 0.0f), {400.0f, 300.0f}));
  CHECK(near(p.worldToScreen({10.0f, -5.0f}, 0.0f), {410.0f, 295.0f}));

  // Elevation is ignored in 2D.
  CHECK(near(p.worldToScreen({10.0f, -5.0f}, 999.0f), {410.0f, 295.0f}));

  // Zoom scales world units to pixels; worldUnitToPixels reports it.
  p.zoom = 2.0f;
  CHECK(near(p.worldToScreen({10.0f, 0.0f}, 0.0f), {420.0f, 300.0f}));
  CHECK(std::fabs(p.worldUnitToPixels() - 2.0f) < 1e-6f);

  // screenToWorld is the inverse of worldToScreen, with a panned camera.
  p.cameraCenter = {123.0f, -45.0f};
  p.zoom = 1.5f;
  const glm::vec2 world{17.0f, 88.0f};
  CHECK(near(p.screenToWorld(p.worldToScreen(world, 0.0f), 0.0f), world));

  if (g_failures == 0)
    std::fprintf(stderr, "flatProjectionTests: all %d checks passed\n", g_passed);
  else
    std::fprintf(stderr, "flatProjectionTests: %d FAILED, %d passed\n",
                 g_failures, g_passed);

  return g_failures == 0 ? 0 : 1;
}
