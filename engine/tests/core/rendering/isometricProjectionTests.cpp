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

TerrainElevationGridView gridView(const std::vector<int>& cells, int w, int h)
{
  TerrainElevationGridView grid;
  grid.elevations = cells.data();
  grid.width = w;
  grid.height = h;
  grid.stride = w;
  grid.origin = {0, 0};
  return grid;
}
} // namespace

int main()
{
  IsometricProjection p = makeProjection();

  TEST("the grid origin should map to the screen centre")
  {
    CHECK(nearVec(p.worldToScreen({0.0f, 0.0f}, 0.0f), {400.0f, 300.0f}));
  }

  TEST("elevation should lift a point straight up")
  {
    const glm::vec2 ground = p.worldToScreen({0.0f, 0.0f}, 0.0f);
    const glm::vec2 raised = p.worldToScreen({0.0f, 0.0f}, 2.0f);
    CHECK(testing::approx(raised.x, ground.x));
    // 2 levels * 8 px/level * scale * zoom = 16 px upward (screen -y)
    CHECK(testing::approx(ground.y - raised.y, 16.0f));
  }

  TEST("screenToWorld should invert worldToScreen")
  {
    const glm::vec2 world{3.0f, 5.0f};
    const float elevation = 2.0f;
    const glm::vec2 screen = p.worldToScreen(world, elevation);
    CHECK(nearVec(p.screenToWorld(screen, elevation), world));
  }

  TEST("worldUnitToPixels should track tile width, scale, and zoom")
  {
    CHECK(testing::approx(p.worldUnitToPixels(), 64.0f));
    p.zoom = 2.0f;
    CHECK(testing::approx(p.worldUnitToPixels(), 128.0f));
    p.zoom = 1.0f;
  }

  TEST("pickTile should resolve the tile under the cursor on flat ground")
  {
    const std::vector<int> cells(16, 0); // 4x4 grid, all ground level 0
    const TerrainElevationGridView grid = gridView(cells, 4, 4);
    CHECK(grid.valid());

    const glm::vec2 screen = p.worldToScreen({2.2f, 1.7f}, 0.0f);
    const TilePick pick = pickTile(screen, p, grid);
    CHECK(pick.valid);
    CHECK(pick.tile == glm::ivec2(2, 1));
    CHECK(pick.elevation == 0);
  }

  TEST("pickTile should resolve a raised column to its top face")
  {
    std::vector<int> cells(9, 0); // 3x3
    cells[1 * 3 + 1] = 2;         // tile (1,1) is 2 levels tall
    const TerrainElevationGridView grid = gridView(cells, 3, 3);

    const glm::vec2 screen = p.worldToScreen({1.5f, 1.5f}, 2.0f);
    const TilePick pick = pickTile(screen, p, grid);
    CHECK(pick.valid);
    CHECK(pick.tile == glm::ivec2(1, 1));
    CHECK(pick.elevation == 2);
  }

  TEST("pickTile should fall back to invalid ground off the grid")
  {
    const std::vector<int> cells(16, 0);
    const TerrainElevationGridView grid = gridView(cells, 4, 4);

    const glm::vec2 screen = p.worldToScreen({50.0f, 50.0f}, 0.0f);
    const TilePick pick = pickTile(screen, p, grid);
    CHECK(!pick.valid);
  }

  return testing::report("isometricProjectionTests");
}
