// Tests for IsometricProjection: the world<->screen transform of the isometric
// heightfield path, and pickTile (topmost terrain tile under a screen point).

#include "../../testHarness.h"

#include <engine/core/rendering/projection/isometricProjection.h>

#include <vector>

using namespace sfs;

namespace
{
IsometricProjection makeProjection()
{
  IsometricProjection p;
  p.tileWidth = 64;
  p.tileHeight = 32;
  p.elevationStep = 8;
  p.worldScale = 1.0f;
  p.zoom = 1.0f;
  p.cameraIso = {0.0f, 0.0f};
  p.screenCenter = {400.0f, 300.0f};
  return p;
}

bool nearVec(const glm::vec2& a, const glm::vec2& b, float eps = 1e-3f)
{
  return testing::approx(a.x, b.x, eps) && testing::approx(a.y, b.y, eps);
}
} // namespace

int main()
{
  IsometricProjection p = makeProjection();

  // --- grid origin maps to the screen centre ------------------------------
  CHECK(nearVec(p.worldToScreen({0.0f, 0.0f}, 0.0f), {400.0f, 300.0f}));

  // --- elevation lifts the point straight up (screen -y) ------------------
  {
    const glm::vec2 ground = p.worldToScreen({0.0f, 0.0f}, 0.0f);
    const glm::vec2 raised = p.worldToScreen({0.0f, 0.0f}, 2.0f);
    CHECK(testing::approx(raised.x, ground.x));
    // 2 levels * 8 px/level * scale * zoom = 16 px upward
    CHECK(testing::approx(ground.y - raised.y, 16.0f));
  }

  // --- screenToWorld is the inverse of worldToScreen ----------------------
  {
    const glm::vec2 world{3.0f, 5.0f};
    const float elevation = 2.0f;
    const glm::vec2 screen = p.worldToScreen(world, elevation);
    CHECK(nearVec(p.screenToWorld(screen, elevation), world));
  }

  // --- worldUnitToPixels tracks tile width * scale * zoom -----------------
  CHECK(testing::approx(p.worldUnitToPixels(), 64.0f));
  p.zoom = 2.0f;
  CHECK(testing::approx(p.worldUnitToPixels(), 128.0f));
  p.zoom = 1.0f;

  // --- pickTile on a flat grid resolves the tile under the cursor ---------
  {
    std::vector<int> elevations(16, 0); // 4x4 grid, all ground level 0
    TerrainElevationGridView grid;
    grid.elevations = elevations.data();
    grid.width = 4;
    grid.height = 4;
    grid.stride = 4;
    grid.origin = {0, 0};
    CHECK(grid.valid());

    const glm::vec2 screen = p.worldToScreen({2.2f, 1.7f}, 0.0f);
    const TilePick pick = pickTile(screen, p, grid);
    CHECK(pick.valid);
    CHECK(pick.tile == glm::ivec2(2, 1));
    CHECK(pick.elevation == 0);
  }

  // --- pickTile resolves a raised column to its top face ------------------
  {
    std::vector<int> elevations(9, 0); // 3x3
    elevations[1 * 3 + 1] = 2;         // tile (1,1) is 2 levels tall
    TerrainElevationGridView grid;
    grid.elevations = elevations.data();
    grid.width = 3;
    grid.height = 3;
    grid.stride = 3;
    grid.origin = {0, 0};

    const glm::vec2 screen = p.worldToScreen({1.5f, 1.5f}, 2.0f);
    const TilePick pick = pickTile(screen, p, grid);
    CHECK(pick.valid);
    CHECK(pick.tile == glm::ivec2(1, 1));
    CHECK(pick.elevation == 2);
  }

  // --- pickTile off the grid falls back to invalid ground -----------------
  {
    std::vector<int> elevations(16, 0);
    TerrainElevationGridView grid;
    grid.elevations = elevations.data();
    grid.width = 4;
    grid.height = 4;
    grid.stride = 4;
    grid.origin = {0, 0};

    const glm::vec2 screen = p.worldToScreen({50.0f, 50.0f}, 0.0f);
    const TilePick pick = pickTile(screen, p, grid);
    CHECK(!pick.valid);
  }

  return testing::report("isometricProjectionTests");
}
