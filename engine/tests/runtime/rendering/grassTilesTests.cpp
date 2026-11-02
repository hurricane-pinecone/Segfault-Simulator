#include "../../testHarness.h"

#include <engine/core/rendering/grass.h>
#include <engine/runtime/rendering/framePlan.h>
#include <engine/runtime/rendering/grassTiles.h>

#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/ext/matrix_transform.hpp"

#include <algorithm>
#include <vector>

using namespace sfs;

namespace
{

// Flat ground at height 0 over a square `half` either side of the origin.
GrassHeights flatGround(float half)
{
  const GrassGrid grid = GrassGrid::over({-half, -half}, {half, half}, 0.25f);
  std::vector<glm::vec4> texels(
      grid.count(), glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
  return GrassHeights(grid, texels);
}

// A meadow's grow pass from a camera at `eye` looking at `at`.
GrassGrowUniforms growFrom(glm::vec3 eye, glm::vec3 at, float half)
{
  GrassBatch batch;
  batch.shape.density = 400.0f;
  batch.drawDistance = 200.0f;
  FrameParams frame;
  frame.viewProjection =
      glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f) *
      glm::lookAt(eye, at, glm::vec3(0.0f, 0.0f, 1.0f));
  frame.cameraPosition = eye;
  GrassFieldState field;
  field.groundGrid = GrassGrid::over({-half, -half}, {half, half}, 0.25f);
  field.groundArea = 4.0f * half * half;
  const std::optional<GrassPlan> plan = planGrass(batch, field, frame);
  return plan ? plan->grow : GrassGrowUniforms{};
}

bool hasTile(const std::vector<GrassTileEntry>& entries,
             int lattice,
             glm::ivec2 origin)
{
  return std::any_of(entries.begin(),
                     entries.end(),
                     [&](const GrassTileEntry& entry)
                     {
                       return (entry.lattice & 3u) == uint32_t(lattice) &&
                              entry.origin == origin;
                     });
}

} // namespace

int main()
{
  TEST("a tile's levels should keep fewer of its lattice's blades each step, "
       "and enough for what's needed")
  {
    for (int lattice = 0; lattice < 4; ++lattice)
    {
      const glm::vec2 ranks = grassLatticeRanks(lattice);
      CHECK(grassTileThreshold(lattice, 0) == ranks.y);
      CHECK(grassTileLevel(lattice, ranks.y * 2.0f) == 0);
      for (float needed = ranks.y; needed > ranks.y * 0.01f; needed *= 0.9f)
      {
        if (needed <= ranks.x)
          break;
        const int level = grassTileLevel(lattice, needed);
        CHECK(grassTileThreshold(lattice, level) >= needed);
        CHECK(level == (lattice < 3 ? 3 : 20) ||
              grassTileThreshold(lattice, level + 1) < needed);
      }
    }
    // The finer lattices' ranks above the coarser ones'; the coarsest to 0.
    CHECK(grassLatticeRanks(0) == glm::vec2(0.25f, 1.0f));
    CHECK(grassLatticeRanks(1).y == grassLatticeRanks(0).x);
    CHECK(grassLatticeRanks(3) == glm::vec2(0.0f, 1.0f / 64.0f));
  }

  TEST("a tile's room should fit about the share of its blades kept, in "
       "chunks, and never more than it has")
  {
    CHECK(grassTileRoom(0, 1.0f) == 3072u);
    CHECK(grassTileRoom(3, 1.0f / 64.0f) == 4096u);
    // Half its ranks.
    const glm::vec2 ranks = grassLatticeRanks(1);
    const uint32_t half = grassTileRoom(1, 0.5f * (ranks.x + ranks.y));
    CHECK(half % kGrassChunk == 0u && half > 1536u && half < 1536u + 512u);
    CHECK(grassTileRoom(2, grassLatticeRanks(2).x) == kGrassChunk);
  }

  TEST("the chunk pool should give out room first fit, join it up as it's "
       "given back, and grow")
  {
    GrassChunkPool pool(10);
    const auto a = pool.allocate(4);
    const auto b = pool.allocate(4);
    CHECK(a && *a == 0u && b && *b == 4u && pool.used() == 8u);
    CHECK(!pool.allocate(4));
    pool.free(*a, 4);
    const auto c = pool.allocate(3);
    CHECK(c && *c == 0u);
    pool.free(*c, 3);
    pool.free(*b, 4);
    CHECK(pool.used() == 0u);
    const auto all = pool.allocate(10); // joined up again
    CHECK(all && *all == 0u);
    pool.grow(16);
    const auto more = pool.allocate(6);
    CHECK(more && *more == 10u && pool.used() == 16u && pool.size() == 16u);
  }

  TEST("a field's tiles should be built once, as they come into view, and "
       "again only where they change")
  {
    GrassTileCache cache;
    cache.setGround(flatGround(100.0f));
    const glm::vec3 eye(0.0f, -40.0f, 6.0f);
    const GrassGrowUniforms grow = growFrom(eye, glm::vec3(0.0f), 100.0f);

    // The first frame: every tile in view built, and grown from.
    const GrassTileCache::Frame& first = cache.plan(grow, 1, false);
    CHECK(!first.drawn.empty());
    CHECK(first.builds.size() == first.drawn.size());
    CHECK(first.outOfRoom == 0u);
    const std::size_t drawn = first.drawn.size();
    uint32_t chunks = 0;
    for (const GrassTileEntry& entry : first.drawn)
    {
      CHECK(entry.firstChunk == chunks);
      CHECK(entry.room % kGrassChunk == 0u && entry.room > 0u);
      // (Its ground's heights, for telling whether it's hidden: flat.)
      CHECK(entry.heights.x <= 0.0f && entry.heights.y >= 0.0f);
      chunks += entry.room / kGrassChunk;
    }
    CHECK(first.chunks == chunks);
    // The tile under the camera's gaze, on the finest lattice, is one.
    CHECK(hasTile(first.drawn, 0, {0, 0}) || hasTile(first.drawn, 0, {-64, 0}));
    // Behind the camera, none.
    CHECK(!hasTile(first.drawn, 3, {-64, -320}));

    // The camera still: nothing built.
    const GrassTileCache::Frame& still = cache.plan(grow, 2, false);
    CHECK(still.builds.empty());
    CHECK(still.drawn.size() == drawn);

    // An edit: only the tiles over it built again, one a lattice.
    cache.changed({1.0f, 1.0f}, {1.5f, 1.5f});
    const GrassTileCache::Frame& edited = cache.plan(grow, 3, true);
    CHECK(edited.builds.size() <= 4u && !edited.builds.empty());
    for (const GrassTileEntry& entry : edited.builds)
    {
      CHECK((entry.lattice & kGrassTileEdited) != 0u);
      CHECK(entry.origin == glm::ivec2(0));
    }

    // Moved on: only the tiles coming into view (or needing more blades)
    // built.
    const GrassGrowUniforms moved = growFrom(
        eye + glm::vec3(4.0f, 0.0f, 0.0f), glm::vec3(4.0f, 0.0f, 0.0f), 100.0f);
    const GrassTileCache::Frame& next = cache.plan(moved, 4, false);
    CHECK(!next.builds.empty());
    CHECK(next.builds.size() < next.drawn.size() / 2);

    // Its placement changed: all of them again.
    GrassGrowUniforms turned = moved;
    turned.model =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.01f));
    const GrassTileCache::Frame& again = cache.plan(turned, 5, false);
    CHECK(again.builds.size() == again.drawn.size());
  }

  TEST("a field's tiles should keep more blades as the camera nears them, and "
       "let go of those unused a while")
  {
    GrassTileCache::Limits limits;
    limits.unusedFrames = 100;
    GrassTileCache cache(limits);
    cache.setGround(flatGround(150.0f));
    const GrassGrowUniforms far =
        growFrom({0.0f, -120.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 150.0f);
    const GrassTileCache::Frame& seen = cache.plan(far, 1, false);
    const auto lowAt = [&](const std::vector<GrassTileEntry>& entries)
    {
      for (const GrassTileEntry& entry : entries)
        if ((entry.lattice & 3u) == 1u && entry.origin == glm::ivec2(0))
          return entry.threshold;
      return -1.0f;
    };
    // (Its second lattice's tile there: most of its blades thinned out.)
    const float before = lowAt(seen.drawn);
    CHECK(before > 0.0f && before < grassLatticeRanks(1).y);
    const uint32_t used = cache.usedChunks();

    // Nearer: built again, keeping more.
    const GrassGrowUniforms near =
        growFrom({0.0f, -8.0f, 3.0f}, {0.0f, 20.0f, 0.0f}, 150.0f);
    const GrassTileCache::Frame& close = cache.plan(near, 2, false);
    CHECK(lowAt(close.drawn) > before);
    CHECK(cache.usedChunks() > 0u && used > 0u);

    // Looking away for long enough: let go.
    GrassGrowUniforms away = near;
    away.lattices = {};
    for (uint64_t frame = 3; frame <= 240; ++frame)
      cache.plan(away, frame, false);
    CHECK(cache.tiles() == 0u && cache.usedChunks() == 0u);
  }

  TEST("ground's heights should be found over any rectangle of it")
  {
    const GrassGrid grid = GrassGrid::over({0.0f, 0.0f}, {8.0f, 8.0f}, 0.25f);
    std::vector<glm::vec4> texels(grid.count(), glm::vec4(0.0f));
    texels[std::size_t(3) * grid.size.x + 3] = glm::vec4(0, 0, 1, 2.0f);
    texels[std::size_t(20) * grid.size.x + 20] = glm::vec4(0, 0, 1, -1.0f);
    const GrassHeights heights(grid, texels);
    CHECK(heights.range({0, 0}, grid.size) == glm::vec2(-1.0f, 2.0f));
    CHECK(heights.range({0, 0}, {4, 4}) == glm::vec2(2.0f, 2.0f));
    const glm::vec2 none = heights.range({24, 0}, {32, 8});
    CHECK(none.x > none.y);
  }

  TEST("maps should tell where what blades grow from changed, block by block, "
       "but not their data")
  {
    GrassMaps maps(GrassGrid::over({0.0f, 0.0f}, {32.0f, 32.0f}, 0.25f));
    // All of it, to start with.
    std::vector<glm::ivec4> changed = maps.takeGrownChanged();
    CHECK(changed.size() == 1u &&
          changed[0] ==
              glm::ivec4(0, 0, maps.grid().size.x, maps.grid().size.y));
    CHECK(maps.takeGrownChanged().empty());

    // Two far apart: two blocks, not all between.
    maps.set(GrassMaps::Coverage, 1, 1, 0.0f);
    maps.set(GrassMaps::Length, 100, 120, 0.5f);
    changed = maps.takeGrownChanged();
    CHECK(changed.size() == 2u);
    CHECK(changed[0] == glm::ivec4(0, 0, 16, 16));
    CHECK(changed[1] == glm::ivec4(96, 112, 112, 128));

    // Side by side: one run.
    maps.set(GrassMaps::Coverage, 15, 40, 0.0f);
    maps.set(GrassMaps::Coverage, 16, 40, 0.0f);
    changed = maps.takeGrownChanged();
    CHECK(changed.size() == 1u && changed[0] == glm::ivec4(0, 32, 32, 48));

    // Data isn't what blades grow from (though it's sent up).
    maps.fill(GrassMaps::Data1, 0.5f);
    maps.stamp(GrassMaps::Data0, {4.0f, 4.0f}, 1.0f, 1.0f);
    CHECK(maps.takeGrownChanged().empty());
    const auto [from, to] = maps.takeChanged();
    CHECK(to == maps.grid().size && from == glm::ivec2(0));
    maps.fill(GrassMaps::Length, 0.5f);
    CHECK(maps.takeGrownChanged().size() == 1u);
  }

  return testing::report("grassTilesTests");
}
