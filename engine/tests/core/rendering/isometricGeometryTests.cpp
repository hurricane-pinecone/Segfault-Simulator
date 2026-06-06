#include "../../testHarness.h"

#include <engine/core/components/elevationComponent.h>
#include <engine/core/rendering/util/isometric/geometry.h>

#include <vector>

using namespace sfs;

namespace
{
bool nearVec(const glm::vec2& a, const glm::vec2& b, float eps = 1e-3f)
{
  return testing::approx(a.x, b.x, eps) && testing::approx(a.y, b.y, eps);
}
} // namespace

int main()
{
  // --- grid <-> isometric round-trip --------------------------------------
  {
    const int tw = 64, th = 32;
    const float scale = 1.0f;
    const glm::vec2 grids[] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {3.5f, -2.25f}};
    for (const glm::vec2& g : grids)
    {
      const glm::vec2 iso = gridToIsometric(g, tw, th, scale);
      CHECK(nearVec(isometricTogrid(iso, tw, th, scale), g));
    }
  }

  // --- gridCellOf floors to the containing cell ---------------------------
  {
    CHECK(gridCellOf({2.9f, 1.1f}) == glm::ivec2(2, 1));
    CHECK(gridCellOf({-0.1f, -0.1f}) == glm::ivec2(-1, -1));
  }

  // --- TerrainElevationGridView: validity, bounds, empty cells ------------
  {
    std::vector<int> e = {0, 1, 2, 3};
    TerrainElevationGridView grid;
    grid.elevations = e.data();
    grid.width = 2;
    grid.height = 2;
    grid.stride = 2;
    grid.origin = {10, 20};
    CHECK(grid.valid());

    int out = -999;
    CHECK(grid.tryGet({10, 20}, out) && out == 0);
    CHECK(grid.tryGet({11, 21}, out) && out == 3);
    CHECK(!grid.tryGet({9, 20}, out));  // left of origin
    CHECK(!grid.tryGet({12, 20}, out)); // right of grid

    // an EmptyElevation cell reads as "no tile"
    std::vector<int> withHole = {0, EmptyElevation, 5, 1};
    grid.elevations = withHole.data();
    grid.origin = {0, 0};
    CHECK(!grid.tryGet({1, 0}, out)); // the hole
    CHECK(grid.tryGet({0, 1}, out) && out == 5);
  }

  // --- maxTerrainElevation ignores empty cells ----------------------------
  {
    std::vector<int> e = {0, 4, EmptyElevation, 2};
    TerrainElevationGridView grid;
    grid.elevations = e.data();
    grid.width = 2;
    grid.height = 2;
    grid.stride = 2;
    grid.origin = {0, 0};
    CHECK(maxTerrainElevation(grid) == 4);

    TerrainElevationGridView invalid;
    CHECK(maxTerrainElevation(invalid) == 0);
  }

  // --- pointInConvexQuad --------------------------------------------------
  {
    const glm::vec2 unit[4] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    CHECK(pointInConvexQuad({0.5f, 0.5f}, unit));
    CHECK(!pointInConvexQuad({1.5f, 0.5f}, unit));
    CHECK(!pointInConvexQuad({-0.5f, 0.5f}, unit));
  }

  // --- clipPolygonToTile clips a large quad to the tile's unit square ------
  {
    const glm::vec2 big[4] = {
        {-1.0f, -1.0f}, {2.0f, -1.0f}, {2.0f, 2.0f}, {-1.0f, 2.0f}};
    const ClippedPolygon clipped = clipPolygonToTile(big, {0, 0});
    CHECK(clipped.count >= 4);
    for (int i = 0; i < clipped.count; ++i)
    {
      CHECK(clipped.points[i].x >= -1e-4f && clipped.points[i].x <= 1.0f + 1e-4f);
      CHECK(clipped.points[i].y >= -1e-4f && clipped.points[i].y <= 1.0f + 1e-4f);
    }
  }

  return testing::report("isometricGeometryTests");
}
