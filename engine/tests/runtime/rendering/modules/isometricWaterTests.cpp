#include "../../../testHarness.h"

#include <engine/core/components/elevationComponent.h>
#include <engine/core/components/surfaceEffect.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/components/waterTileComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/rendering/projection/isometricProjection.h>
#include <engine/core/rendering/renderPass.h>
#include <engine/runtime/rendering/isometricRenderContext.h>
#include <engine/runtime/rendering/modules/isometricWater.h>

using namespace sfs;

namespace
{
IsometricProjection makeProjection()
{
  IsometricProjection p;
  p.tileWidth = 32;
  p.tileHeight = 16;
  p.elevationStep = 8;
  p.worldScale = 1.0f;
  p.zoom = 1.0f;
  return p;
}

// A water tile: its surface sits at `waterLevel`, the terrain under it at
// `terrainLevel`. Water shows only where its surface is above the terrain.
Entity
addWaterTile(Registry& reg, glm::vec2 pos, int waterLevel, int terrainLevel)
{
  Entity e = reg.createEntity();
  e.addComponent<TransformComponent>(TransformComponent{pos});
  e.addComponent<ElevationComponent>(ElevationComponent{terrainLevel});
  WaterTileComponent water;
  water.elevation = waterLevel;
  e.addComponent<WaterTileComponent>(water);
  return e;
}
} // namespace

int main()
{
  TEST("a submerged tile should mesh one water quad")
  {
    Registry reg;
    addWaterTile(reg, {0.5f, 0.5f}, 2, 0); // surface at 2, ground at 0

    IsometricWater water;
    water.init(ModuleInit{&reg, nullptr});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    water.computeCommands(ctx);

    CHECK(water.commands().size() == 1);
    const SurfaceCommand& cmd = water.commands().front();
    CHECK(cmd.type == SurfaceEffect::Type::Water);
    // Each tile is tessellated into a 4x4 grid (for the per-vertex wave): a
    // 5x5 vertex grid = 25 verts, 16 quads * 2 tris * 3 = 96 indices.
    CHECK(cmd.vertices.size() == 25);
    CHECK(cmd.indices.size() == 96);
    CHECK(cmd.order.pass == RenderPass::Surfaces);
  }

  TEST("water at or below the terrain should not mesh")
  {
    Registry reg;
    addWaterTile(reg, {0.5f, 0.5f}, 2, 5); // ground (5) above the surface (2)

    IsometricWater water;
    water.init(ModuleInit{&reg, nullptr});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    water.computeCommands(ctx);

    CHECK(water.commands().empty());
  }

  TEST("adjacent water tiles should merge into one mesh")
  {
    Registry reg;
    addWaterTile(reg, {0.5f, 0.5f}, 2, 0);
    addWaterTile(reg, {1.5f, 0.5f}, 2, 0);

    IsometricWater water;
    water.init(ModuleInit{&reg, nullptr});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    water.computeCommands(ctx);

    CHECK(water.commands().size() == 1); // all water in one draw
    // Two tessellated tiles (25 verts + 96 indices each).
    CHECK(water.commands().front().vertices.size() == 50);
    CHECK(water.commands().front().indices.size() == 192);
  }

  TEST("no water tiles should mesh nothing")
  {
    Registry reg;
    IsometricWater water;
    water.init(ModuleInit{&reg, nullptr});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    water.computeCommands(ctx);

    CHECK(water.commands().empty());
  }

  TEST("the surface command should carry the scene ambient level")
  {
    Registry reg;
    addWaterTile(reg, {0.5f, 0.5f}, 2, 0);

    IsometricWater water;
    water.init(ModuleInit{&reg, nullptr});
    IsometricProjection proj = makeProjection();
    IsometricAmbientLighting ambient;
    ambient.ambient = 0.5f;
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    ctx.ambientLighting = &ambient;
    water.computeCommands(ctx);

    CHECK(testing::approx(water.commands().front().ambient, 0.5f));
  }

  return testing::report("isometricWaterTests");
}
