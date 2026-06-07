#include "../../../testHarness.h"

#include <engine/core/components/elevationComponent.h>
#include <engine/core/components/spriteComponent.h>
#include <engine/core/components/surfaceEffect.h>
#include <engine/core/components/tags/isometricTile.h>
#include <engine/core/components/terrainBoundaryComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/rendering/projection/isometricProjection.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/isometricRenderContext.h>
#include <engine/runtime/rendering/modules/blockGeometry.h>

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

// A terrain tile entity BlockGeometry meshes: transform + elevation + sprite +
// the IsometricTile tag.
Entity addTile(Registry& reg, AssetStore& store, glm::vec2 pos, int level,
               SpriteId sprite)
{
  Entity e = reg.createEntity();
  e.addComponent<TransformComponent>(TransformComponent{pos});
  e.addComponent<ElevationComponent>(ElevationComponent{level});
  e.addComponent<SpriteComponent>(SpriteComponent{sprite});
  e.addTag<IsometricTile>();
  return e;
}

SpriteId registerCube(AssetStore& store, const std::string& texture)
{
  store.addRadialTexture(texture, 32); // a 32x32 CPU surface for the material
  return store.addSprite(texture, texture + "_spr", SDL_Rect{0, 0, 32, 32});
}
} // namespace

int main()
{
  TEST("a flat tile should mesh a single top face")
  {
    Registry reg;
    AssetStore store;
    const SpriteId cube = registerCube(store, "block");
    addTile(reg, store, {0.0f, 0.0f}, 1, cube);

    BlockGeometry geo;
    geo.init(ModuleInit{&reg, &store});

    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    geo.computeCommands(ctx);

    CHECK(geo.commands().size() == 1);
    const GeometryCommand& cmd = geo.commands().front();
    CHECK(cmd.vertices.size() == 6); // top quad = two triangles
    CHECK(cmd.textureId != nullptr);
    CHECK(*cmd.textureId == "block");
    CHECK(cmd.type == SurfaceEffect::Type::None);
  }

  TEST("an exposed boundary tile should add side faces")
  {
    Registry reg;
    AssetStore store;
    const SpriteId cube = registerCube(store, "block");
    Entity tile = addTile(reg, store, {0.0f, 0.0f}, 1, cube);

    TerrainBoundaryComponent boundary;
    boundary.southExposed = true;
    boundary.southBottomElevation = 0; // one level of south face
    boundary.eastExposed = true;
    boundary.eastBottomElevation = 0; // one level of east face
    tile.addComponent<TerrainBoundaryComponent>(boundary);

    BlockGeometry geo;
    geo.init(ModuleInit{&reg, &store});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    geo.computeCommands(ctx);

    CHECK(geo.commands().size() == 1);
    // top (6) + south level (6) + east level (6).
    CHECK(geo.commands().front().vertices.size() == 18);
  }

  TEST("tiles sharing a texture should merge into one command")
  {
    Registry reg;
    AssetStore store;
    const SpriteId cube = registerCube(store, "block");
    addTile(reg, store, {0.0f, 0.0f}, 1, cube);
    addTile(reg, store, {1.0f, 0.0f}, 1, cube);

    BlockGeometry geo;
    geo.init(ModuleInit{&reg, &store});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    geo.computeCommands(ctx);

    CHECK(geo.commands().size() == 1);       // one material bucket
    CHECK(geo.commands().front().vertices.size() == 12); // two top faces
  }

  TEST("tiles with different textures should split into separate commands")
  {
    Registry reg;
    AssetStore store;
    addTile(reg, store, {0.0f, 0.0f}, 1, registerCube(store, "block"));
    addTile(reg, store, {1.0f, 0.0f}, 1, registerCube(store, "block2"));

    BlockGeometry geo;
    geo.init(ModuleInit{&reg, &store});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    geo.computeCommands(ctx);

    CHECK(geo.commands().size() == 2); // one bucket per material
  }

  TEST("no tiles should emit no geometry")
  {
    Registry reg;
    AssetStore store;
    BlockGeometry geo;
    geo.init(ModuleInit{&reg, &store});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    geo.computeCommands(ctx);

    CHECK(geo.commands().empty());
  }

  TEST("a missing projection should emit no geometry")
  {
    Registry reg;
    AssetStore store;
    const SpriteId cube = registerCube(store, "block");
    addTile(reg, store, {0.0f, 0.0f}, 1, cube);

    BlockGeometry geo;
    geo.init(ModuleInit{&reg, &store});
    IsometricRenderContext ctx; // projection left null
    geo.computeCommands(ctx);

    CHECK(geo.commands().empty());
  }

  return testing::report("blockGeometryTests");
}
